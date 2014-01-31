#ifndef _CONFIG__H
#define _CONFIG__H 1

#include <yaml.h>

enum err {
    OK = 0,
    PARSER_SYNTAX,
};

struct cfg {
    int ttl;
    enum err err;
    yaml_parser_t parser;
};

extern struct cfg *cfg_load(const char *fname);

#endif /* _CONFIG__H */

