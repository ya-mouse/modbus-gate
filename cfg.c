#include <yaml.h>
#include <stdio.h>

#include "mbus-gw.h"
#include "cfg.h"

#define GET_STRING(val) \
    if (!(v = cfg_get_string(&cfg->parser, val, &event))) { \
        yaml_event_delete(&event);                          \
        if (val == NULL) {                                  \
            break;                                          \
        else                                                \
            v = val;                                        \
    }

static int get_speed(const char *baud, int defval)
{
    int spd;

    if (!strcmp(baud, "9600"))
        spd = B9600;
    else if (!strcmp(baud, "19200"))
        spd = B19200;
    else if (!strcmp(baud, "38400"))
        spd = B38400;
    else if (!strcmp(baud, "57600"))
        spd = B57600;
    else if (!strcmp(baud, "115200"))
        spd = B115200;
    else
        spd = defval;

    return spd;
}

#ifndef _NUTTX_BUILD
static void cfg_expect_event(struct cfg *cfg, const enum yaml_event_type_e type)
{
    yaml_event_t event;

    if (cfg->err)
        return;

    yaml_parser_parse(&cfg->parser, &event);
    if (event.type != type) {
        cfg->err = PARSER_SYNTAX;
        fprintf(stderr, "Syntax error\n");
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

static char *cfg_get_string(struct cfg *cfg, char *def, yaml_event_t *event)
{
    char *v;

    if (cfg->err)
        return def;

    yaml_event_delete(event);

    yaml_parser_parse(&cfg->parser, event);
    if (event->type != YAML_SCALAR_EVENT) {
        cfg->err = PARSER_SYNTAX;
        v = def;
    } else {
        v = (char *)event->data.scalar.value;
    }

    return v;
}

static void cfg_parse_map(struct cfg *cfg, struct rtu_desc *r)
{
    int i;
    char *v;
    struct slave_map map;
    yaml_event_t event;

    if (cfg->err)
        return;

    map.src = -1;
    map.dst = -1;

    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_SCALAR_EVENT:
            v = (char *)event.data.scalar.value;
            i = cfg_get_int(cfg, -1);
            if (!strcmp(v, "src")) {
                map.src = i;
            } else if (!strcmp(v, "dst")) {
                map.dst = i;
            }
            break;

        case YAML_MAPPING_END_EVENT:
            if (map.src == -1 || map.dst == -1) {
                cfg->err = MISSED_VALUE;
            } else {
                VADD(r->slave_id, map);
            }
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            fprintf(stderr, "Unknown elem %d\n", event.type);
            break;
        }
        yaml_event_delete(&event);
    }
}

static void cfg_parse_map_list(struct cfg *cfg, struct rtu_desc *r)
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
            cfg_parse_map(cfg, r);
            break;

        case YAML_SEQUENCE_END_EVENT:
            if (!VLEN(r->slave_id)) {
                cfg->err = MISSED_VALUE;
                fprintf(stderr, "At least one map is required\n");
            }
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            fprintf(stderr, "Unknown elem %d\n", event.type);
            break;
        }
        yaml_event_delete(&event);
    }
}

static void cfg_parse_rtu(struct cfg *cfg)
{
    int iv;
    char *v;
    struct rtu_desc r;
    yaml_event_t event;

    if (cfg->err)
        return;

    memset(&r, 0, sizeof(struct rtu_desc));

    VINIT(r.slave_id);

    for (;;) {
        if (cfg->err)
            return;

        yaml_parser_parse(&cfg->parser, &event);
        switch (event.type) {
        case YAML_SCALAR_EVENT:
            v = (char *)event.data.scalar.value;
            if (!strcmp(v, "type")) {
                if (!(v = cfg_get_string(cfg, NULL, &event)))
                    break;

                if (r.type != NONE) {
                    cfg->err = INVALID_PARAM;
                    fprintf(stderr, "RTU TYPE is already set\n");
                    break;
                }

                if (!strcasecmp(v, "modbus-rtu")) {
                    r.type = RTU;
                } else if (!strcasecmp(v, "modbus-tcp")) {
                    r.type = TCP;
                    r.cfg.tcp.port = 502;
#ifndef _NUTTX_BUILD
                } else if (!strcasecmp(v, "modbus-realcom")) {
                    r.type = REALCOM;
                    r.cfg.realcom.port = -1;
                    r.cfg.realcom.cmdfd = -1;
                    r.fd = -1;
                    r.cfg.realcom.t.c_cflag = CS8 | B9600;
#endif
                } else {
                    cfg->err = UNKNOWN_VALUE;
                    fprintf(stderr, "Unkown TYPE: %s\n", v);
                }
            } else if (!strcmp(v, "map")) {
                cfg_parse_map_list(cfg, &r);
            } else if (!strcmp(v, "device")) {
                if (!(v = cfg_get_string(cfg, NULL, &event)))
                    break;

                if (r.type == RTU) {
                    r.cfg.serial.devname = strdup(v);
                } else {
                    cfg->err = INVALID_PARAM;
                    fprintf(stderr, "Invalid param DEVICE for the RTU\n");
                }
            } else if (!strcmp(v, "host")) {
                if (!(v = cfg_get_string(cfg, NULL, &event)))
                    break;

                if (r.type == TCP
#ifndef _NUTTX_BUILD
                    || r.type == REALCOM
#endif
                   ) {
                    r.cfg.tcp.hostname = strdup(v);
                } else {
                    cfg->err = INVALID_PARAM;
                    fprintf(stderr, "Invalid param HOST for the RTU\n");
                }
            } else if (!strcmp(v, "port")) {
                iv = cfg_get_int(cfg, -1);
                if ((r.type == TCP
#ifndef _NUTTX_BUILD
                     || r.type == REALCOM
#endif
                    ) && iv != -1) {
                    r.cfg.tcp.port = 950 + iv - 1;
                } else {
                    cfg->err = INVALID_PARAM;
                    fprintf(stderr, "Invalid param PORT for the RTU\n");
                }
            } else if (!strcmp(v, "timeout")) {
                iv = cfg_get_int(cfg, -1);
                r.timeout = iv;
            } else if (!strcmp(v, "baud")) {
                int spd;

                if (!(v = cfg_get_string(cfg, NULL, &event)))
                    break;

                spd = get_speed(v, B9600);

                if (r.type == RTU) {
                    r.cfg.serial.t.c_cflag = CS8 | spd;
#ifndef _NUTTX_BUILD
                } else if (r.type == REALCOM) {
                    r.cfg.realcom.t.c_cflag = CS8 | spd;
#endif
                } else {
                    cfg->err = INVALID_PARAM;
                    fprintf(stderr, "Invalid param BAUD for the RTU\n");
                }
            }
            break;

        case YAML_MAPPING_END_EVENT:
#ifndef _NUTTX_BUILD
            if (r.type == REALCOM) {
                if (r.cfg.realcom.port == -1) {
                    cfg->err = MISSED_VALUE;
                    fprintf(stderr, "PORT value required for RealCOM RTU\n");
                    goto out;
                }
                r.cfg.realcom.cmdport = 16 + r.cfg.realcom.port;
            }
#endif
            if (r.timeout == 0) {
                r.timeout = RTU_TIMEOUT;
            }
            VADD(cfg->rtu_list, r);

            memset(&r, 0, sizeof(struct rtu_desc));

out:
            yaml_event_delete(&event);
            return;

        default:
            cfg->err = PARSER_SYNTAX;
            fprintf(stderr, "Unknown elem %d\n", event.type);
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

    VINIT(cfg->rtu_list);

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
            fprintf(stderr, "Unknown elem %d\n", event.type);
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
            if (!strcmp(v, "ttl")) {
                cfg->ttl = cfg_get_int(cfg, CFG_DEFAULT_TTL);
            } else if (!strcmp(v, "workers")) {
                cfg->workers = cfg_get_int(cfg, CFG_DEFAULT_WORKERS);
            } else if (!strcmp(v, "socket")) {
                if (!(v = cfg_get_string(cfg, NULL, &event)))
                    break;
                cfg->sockfile = strdup(v);
            } else if (!strcmp(v, "rtu")) {
                cfg_parse_rtu_list(cfg);
            } else if (!strcmp(v, "baud")) {
                if (!(v = cfg_get_string(cfg, NULL, &event)))
                    break;
                cfg->baud = get_speed(v, B9600);
            }
            break;

        case YAML_MAPPING_END_EVENT:
            yaml_event_delete(&event);
            return;

        default:
            // Syntax error
            cfg->err = PARSER_SYNTAX;
            fprintf(stderr, "first_layer: Unknown elem %d\n", event.type);
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
#endif

void cfg_free(struct cfg *cfg)
{
    free(cfg->sockfile);
    free(cfg);
}

struct cfg *cfg_load(const char *fname)
{
    FILE *fp;
    struct cfg *cfg = NULL;

#if 0
    fp = fopen(fname, "r");
    if (!fp) {
        return cfg;
    }
#endif

    cfg = calloc(1, sizeof(struct cfg));
    cfg->workers = 1; //CFG_DEFAULT_WORKERS;
    cfg->ttl = CFG_DEFAULT_TTL;
    cfg->sockfile = strdup(CFG_DEFAULT_SOCKFILE);

#if 0
    yaml_parser_initialize(&cfg->parser);
    yaml_parser_set_input_file(&cfg->parser, fp);
    cfg_parse_config(cfg);
    yaml_parser_delete(&cfg->parser);

    fclose(fp);
#else
    VINIT(cfg->rtu_list);
    {
      struct rtu_desc r;
      struct slave_map map;
      memset(&r, 0, sizeof(struct rtu_desc));
      VINIT(r.slave_id);
      r.type = RTU;
      r.cfg.serial.devname = strdup("/dev/ttyS1");
      r.cfg.serial.t.c_cflag = CS8 | B9600;
      map.src = 1;
      map.dst = 1;
      VADD(r.slave_id, map);
      VADD(cfg->rtu_list, r);
    }
#endif

    if (cfg->err) {
        cfg_free(cfg);
        cfg = NULL;
    }
    return cfg;
}
