#ifndef _MBUS_GW__H
#define _MBUS_GW__H 1

#include <linux/termios.h>
#include <linux/serial.h>
#include <sys/types.h>
#include "vect.h"

//#undef DEBUG
#define DEBUG
#ifdef DEBUG
#define DEBUGF(x...) printf(x)
#else
#define DEBUGF(x...) do { } while(0);
#endif

#define CHILD_NUM       4
#define BUF_SIZE        512
#define MAX_EVENTS      1024
#define MODBUS_TCP_PORT 502
#define RTU_TIMEOUT     3
#define CACHE_TTL       1

enum rtu_type {
    ASCII,
    RTU,
    TCP,
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
};

typedef QUEUE(struct queue_list) queue_list_v;

struct slave_map {
    u_int16_t src;
    u_int16_t dst;
};

typedef VECT(struct slave_map) slave_map_v;

struct rtu_desc {
    int fd;                 /* ttySx descriptior */
    enum rtu_type type;     /* endpoint RTU device type */
    slave_map_v slave_id;   /* slave_id configured for MODBUS-TCP */
    u_int16_t tid;
    u_int8_t tido[2];
    union {
        struct {
            char *hostname;     /* MODBUS-TCP hostname */
            int port;
        } tcp;
        struct {
            char *hostname;     /* Moxa RealCOM */
            int port;
            int cmdport;
            int cmdfd;
            struct termio t;
            int flags;
            int modem_control;
        } realcom;
        struct {
            char *devname;  /* serial device name */
            struct termio t;
        } serial;
    } cfg;
    queue_list_v q;         /* queue list */
    struct cache_page *p;   /* cache pages */
};

typedef VECT(struct rtu_desc) rtu_desc_v;

struct childs {
    int n;
    int ep;
    pthread_t th;
};

#endif /* _MBUS_GW__H */
