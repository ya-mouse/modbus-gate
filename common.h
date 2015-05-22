#ifndef _MBUS_COMMON__H
#define _MBUS_COMMON__H 1

#include <termios.h>
#include "vect.h"

//#undef DEBUG
#define DEBUG
#ifdef DEBUG
#define DEBUGF(x...) printf(x)
#else
#define DEBUGF(x...) do { } while(0);
#endif

#define BUF_SIZE        512
#define MAX_EVENTS      1024
#define MODBUS_TCP_PORT 502
#define RTU_TIMEOUT     3
#define CACHE_TTL       1

enum rtu_type {
    NONE,
    ASCII,
    RTU,
    TCP,
    UNIX,
    REALCOM,
};

struct cache_page {
    u_int8_t status;        /* 0 - ok, 1 - timeout, 2 - NA */
    u_int8_t slaveid;
    u_int16_t addr;
    u_int16_t function;
    u_int16_t len;
    time_t ttd;             /* time to die of the page: last_timestamp + TTL */
    u_int8_t *buf;
    struct cache_page *next;
    struct cache_page *prev;
};

struct queue_list {
    int resp_fd;            /* "response to" descriptor */
    u_int8_t *buf;          /* request buffer */
    size_t len;             /* request length */
    time_t stamp;           /* timestamp of timeout: last_timestamp + timeout */
    int16_t src;            /* source slave_id */
};

typedef QUEUE(struct queue_list) queue_list_v;

struct slave_map {
    int16_t src;
    int16_t dst;
};

typedef VECT(struct slave_map) slave_map_v;

struct rtu_desc {
    int fd;                 /* ttySx descriptior */
    int retries;            /* number of retries */
    long timeout;           /* timeout in seconds */
    enum rtu_type type;     /* endpoint RTU device type */
    u_int16_t tid;
    slave_map_v slave_id;   /* slave_id configured for MODBUS-TCP */
    union {
#define RTU_CFG_COMMON       \
            char *hostname;  \
            int port
        struct {
            RTU_CFG_COMMON;
        } tcp;
        struct {
            char *sockfile;
        } name;
        struct {
            RTU_CFG_COMMON;
            int cmdport;
            int cmdfd;
            struct termios t;
            int flags;
            int modem_control;
        } realcom;
        struct {
            char *devname;  /* serial device name */
            struct termios t;
        } serial;
#undef RTU_CFG_COMMON
    } cfg;

    /* Master-related stuff */
    u_int8_t tido[2];
    queue_list_v q;         /* queue list */
    struct cache_page *p;   /* cache pages */
    int16_t toread;      /* number of words (2-bytes) to read for RTU */
};

typedef VECT(struct rtu_desc) rtu_desc_v;

struct workers {
    int n;
    int ep;
    pthread_t th;
    struct cfg *cfg;
};

extern u_int16_t crc16(const u_int8_t *data, int len);

#endif /* _MBUS_COMMON__H */
