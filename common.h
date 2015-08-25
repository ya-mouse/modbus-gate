#ifndef _MBUS_COMMON__H
#define _MBUS_COMMON__H 1

#include <stdint.h>
#include <termios.h>
#include <pthread.h>
#include <sys/time.h>
#include "vect.h"

#undef DEBUG
//#define DEBUG
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

#ifndef SUN_LEN
#define SUN_LEN(ptr) ((size_t) (((struct sockaddr_un *) 0)->sun_path) + strlen ((ptr)->sun_path))
#endif

enum rtu_type {
    NONE,
    ASCII,
    RTU,
    TCP,
    UNIX,
#ifndef _NUTTX_BUILD
    REALCOM,
#endif
};

struct cache_page {
    uint8_t status;        /* 0 - ok, 1 - timeout, 2 - NA */
    uint8_t slaveid;
    uint16_t addr;
    uint16_t function;
    uint16_t len;
    time_t ttd;             /* time to die of the page: last_timestamp + TTL */
    uint8_t *buf;
    struct cache_page *next;
    struct cache_page *prev;
};

struct queue_list {
    int resp_fd;            /* "response to" descriptor */
    uint8_t *buf;           /* request buffer */
    size_t len;             /* request length */
    time_t stamp;           /* timestamp of timeout: last_timestamp + timeout */
    time_t expire;          /* timestamp of query expiration: last_timestamp + timeout */
    int16_t src;            /* source slave_id */
    uint8_t tido[2];
    uint8_t function;
    uint8_t answered;
    uint8_t requested;
};

struct writeback {
    int ep;                 /* epoll descriptor */
    int fd;                 /* "response to" descriptor */
    uint8_t *buf;           /* request buffer */
    size_t len;             /* request length */
};

typedef VECT(struct queue_list) queue_list_v;

typedef VECT(struct writeback) writeback_v;

struct slave_map {
    int16_t src;
    int16_t dst;
};

typedef VECT(struct slave_map) slave_map_v;

struct rtu_desc {
    int fd;                 /* ttySx descriptior */
    int retries;            /* number of retries */
    long timeout;           /* timeout in seconds */
    int baud;               /* global baud rate */
    enum rtu_type type;     /* endpoint RTU device type */
    uint16_t tid;
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
    uint8_t tido[2];
    queue_list_v q;         /* queue list */
    struct cache_page *p;   /* cache pages */
    int16_t toread;      /* number of words (2-bytes) to read for RTU */
    int16_t toread_off;  /* number of words read */
    uint8_t *toreadbuf;  /* temporary buffer */
    struct timeval tv;   /* last request/answer time */
    struct cfg *conf;
};

typedef VECT(struct rtu_desc) rtu_desc_v;

struct workers {
    int n;
    int ep;
    pthread_t th;
    struct cfg *cfg;
};

extern uint16_t crc16(const uint8_t *data, int len);

#endif /* _MBUS_COMMON__H */
