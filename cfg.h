#ifndef _CONFIG__H
#define _CONFIG__H 1

#include <yaml.h>
#include "mbus-gw.h"

enum err {
    OK = 0,
    PARSER_SYNTAX,
    UNKNOWN_VALUE,
    MISSED_VALUE,
    INVALID_PARAM,
};

struct cfg {
    int ttl;
    rtu_desc_v rtu_list;
    enum err err;
    yaml_parser_t parser;
};

extern struct cfg *cfg_load(const char *fname);

#endif /* _CONFIG__H */

