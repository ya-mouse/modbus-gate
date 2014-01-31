#include <yaml.h>
#include <stdio.h>

#include "mbus-gw.h"
#include "cfg.h"

static void cfg_expect_event(struct cfg *cfg, const enum yaml_event_type_e type)
{
    yaml_event_t event;

    if (cfg->err)
        return;

    yaml_parser_parse(&cfg->parser, &event);
    if (event.type != type) {
        cfg->err = PARSER_SYNTAX;
        printf("Syntax error\n");
    }
    yaml_event_delete(&event);
}

static int cfg_get_int(struct cfg *cfg, int def)
{
    int v;
    yaml_event_t event;

    if (cfg->err)
        return def;

    yaml_parser_parse(&cfg->parser, &event);
    if (event.type != YAML_SCALAR_EVENT) {
        cfg->err = PARSER_SYNTAX;
        v = def;
    } else {
        v = atoi((char *)event.data.scalar.value);
    }
    yaml_event_delete(&event);

    return v;
}

static void cfg_parse_map(struct cfg *cfg)
{
    char *v;
    yaml_event_t event;

    if (cfg->err)
        return;

    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_SCALAR_EVENT:
            v = (char *)event.data.scalar.value;
            printf("scalar %s\n", v);
            break;

        case YAML_MAPPING_END_EVENT:
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            printf("Unknown elem %d\n", event.type);
            break;
        }
        yaml_event_delete(&event);
    }
}

static void cfg_parse_map_list(struct cfg *cfg)
{
    yaml_event_t event;

    if (cfg->err)
        return;

    cfg_expect_event(cfg, YAML_SEQUENCE_START_EVENT);
    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_MAPPING_START_EVENT:
            cfg_parse_map(cfg);
            break;

        case YAML_SEQUENCE_END_EVENT:
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            printf("Unknown elem %d\n", event.type);
            break;
        }
        yaml_event_delete(&event);
    }
}

static void cfg_parse_rtu(struct cfg *cfg)
{
    char *v;
    yaml_event_t event;

    if (cfg->err)
        return;

    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_SCALAR_EVENT:
            v = (char *)event.data.scalar.value;
            printf("scalar %s\n", v);
            if (!strcmp(v, "map")) {
                cfg_parse_map_list(cfg);
            }
            break;

        case YAML_MAPPING_END_EVENT:
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            printf("Unknown elem %d\n", event.type);
            break;
        }
        yaml_event_delete(&event);
    }    
}

static void cfg_parse_rtu_list(struct cfg *cfg)
{
    yaml_event_t event;

    if (cfg->err)
        return;

    cfg_expect_event(cfg, YAML_SEQUENCE_START_EVENT);
    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_MAPPING_START_EVENT:
            cfg_parse_rtu(cfg);
            break;

        case YAML_SEQUENCE_END_EVENT:
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            printf("Unknown elem %d\n", event.type);
            break;
        }
        yaml_event_delete(&event);
    }
}

static void cfg_parse_first_layer(struct cfg *cfg)
{
    char *v;
    yaml_event_t event;

    cfg_expect_event(cfg, YAML_MAPPING_START_EVENT);
    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_SCALAR_EVENT:
            v = (char *)event.data.scalar.value;
            printf("scalar %s\n", v);
            if (!strcmp(v, "ttl")) {
                cfg->ttl = cfg_get_int(cfg, 3);
            } else if (!strcmp(v, "rtu")) {
                cfg_parse_rtu_list(cfg);
            }
            break;

        case YAML_MAPPING_END_EVENT:
            yaml_event_delete(&event);
            return;

        default:
            // Syntax error
            cfg->err = PARSER_SYNTAX;
            printf("first_layer: Unknown elem %d\n", event.type);
            break;
        }

        yaml_event_delete(&event);
    }
}

static void cfg_parse_config(struct cfg *cfg)
{
    cfg_expect_event(cfg, YAML_STREAM_START_EVENT);
    cfg_expect_event(cfg, YAML_DOCUMENT_START_EVENT);
    cfg_parse_first_layer(cfg);
    cfg_expect_event(cfg, YAML_DOCUMENT_END_EVENT);
    cfg_expect_event(cfg, YAML_STREAM_END_EVENT);
}

struct cfg *cfg_load(const char *fname)
{
    FILE *fp;
    struct cfg *cfg = NULL;

    fp = fopen(fname, "r");
    if (!fp) {
        return cfg;
    }

    cfg = calloc(1, sizeof(struct cfg));

    yaml_parser_initialize(&cfg->parser);
    yaml_parser_set_input_file(&cfg->parser, fp);
    cfg_parse_config(cfg);
    yaml_parser_delete(&cfg->parser);

    fclose(fp);

    if (cfg->err) {
        // free fields
        free(cfg);
        cfg = NULL;
    }
    return cfg;
}
