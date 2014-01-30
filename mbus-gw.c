#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

#include "vect.h"

/* Slave address (1-255 for RTU/ASCII, 0-255 for TCP) */

#define CHILD_NUM       4
#define BUF_SIZE        512
#define MAX_EVENTS      1024
#define MODBUS_TCP_PORT 502
#define RTU_TIMEOUT     1
#define CACHE_TTL       3

#define DBL_FREE(e,n)               \
    free(e->buf);                   \
    if (!e->prev) {                 \
        rtu->e = e->next;           \
        if (rtu->e)                 \
            rtu->e->prev = NULL;    \
    } else {                        \
        e->prev->next = e->next;    \
    }                               \
    n = e->next;                    \
    free(e)

enum rtu_type {
    ASCII,
    RTU,
    TCP,
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
    union {
        char *hostname;     /* MODBUS-TCP hostname */
        struct {
            char *devname;  /* serial device name */
            int baudrate;   /* device baudrate */
            int parity;
            int bits;
        } serial;
    } cfg;
    slave_map_v slave_id;   /* slave_id configured for MODBUS-TCP */
    queue_list_v q;         /* queue list */
    struct cache_page *p;   /* cache pages */
};

typedef VECT(struct rtu_desc) rtu_desc_v;

struct childs {
    int n;
    int ep;
    pthread_t th;
};

static rtu_desc_v rtu_list;
static pthread_rwlock_t rwlock;

struct rtu_desc *rtu_by_slaveid(int slave_id)
{
    struct slave_map *mi;
    struct rtu_desc *ri;

    pthread_rwlock_rdlock(&rwlock);
    VFOREACH(rtu_list, ri) {
        VFOREACH(ri->slave_id, mi) {
            if (mi->src == slave_id)
                goto out;
        }
    }
    ri = NULL;

out:
    pthread_rwlock_unlock(&rwlock);

    return ri;
}

struct rtu_desc *rtu_by_fd(int fd)
{
    struct rtu_desc *ri;

    pthread_rwlock_rdlock(&rwlock);
    VFOREACH(rtu_list, ri) {
        if (ri->fd == fd)
            goto out;
    }
    ri = NULL;

out:
    pthread_rwlock_unlock(&rwlock);

    return ri;
}

void cache_update(struct rtu_desc *rtu, const u_int8_t *buf, size_t len)
{
    int slave;
    int addr;
    int nb;
    struct queue_list *q;
    struct cache_page *p;
    struct cache_page *new;

    if (!rtu || !rtu->p || !QLEN(rtu->q))
        return;

    pthread_rwlock_wrlock(&rwlock);
    /* Check for function type */
    // ...

    /* TODO: Write (change register) request shouldn't be cached */

    q = &QGET(rtu->q, 0);
    slave = 1; // q->buf[6];
    addr = 0; //q->buf[8] >> 8 | q->buf[9];
    nb = 4; // q->buf[10] >> 8 | q->buf[11];

    if (!rtu->p) {
//        printf("new cache: ");
        p = rtu->p = calloc(1, sizeof(struct cache_page));
        p->next = NULL;
        p->prev = NULL;
    } else {
        p = rtu->p;
        while (p->next != NULL) {
            if (addr == p->addr && slave == p->slaveid) {
                /* Update page if it has the same constraints */
//                printf("update %p (%d,%d)\n", p, nb, p->len);
                if (nb == p->len)
                    goto update;
                break;
            } else if (addr > p->addr) {
                /* Insert before */
                if (!p->prev) {
                    rtu->p = calloc(1, sizeof(struct cache_page));
                    rtu->p->next = p;
                    p->prev = rtu->p;
                    p = rtu->p;
                    goto fill;
                }
                p = p->prev;
                break;
            }
            p = p->next;
        }
        new = malloc(sizeof(struct cache_page));
        new->next = p->next;
        if (new->next)
            new->next->prev = new;
        p->next = new;
        new->prev = p;
        p = new;
//        printf("alloc %p n=%p p=%p\n", p, new->next, new->prev);
    }

fill:
    p->addr = addr;
    p->slaveid = slave;
    p->buf = malloc(len);
    p->len = len;

update:
    memcpy(p->buf, buf, len);
    /* TODO: TTL have to be configured for each RTU / slave */
    p->ttd = time(NULL) + CACHE_TTL;
    pthread_rwlock_unlock(&rwlock);
}

struct cache_page *_page_free(struct rtu_desc *rtu, struct cache_page *p)
{
    struct cache_page *next;

    if (!rtu || !p)
        return NULL;

//    printf("free: %p ", p);
    DBL_FREE(p, next);

//    printf(" n=%p\n", next);
    return next;
}

struct cache_page *_cache_find(struct rtu_desc *rtu, struct queue_list *q)
{
    int slave;
    int addr;
    int nb;
    struct cache_page *p;

    if (!rtu)
        return NULL;

    /* TODO: Write (change register) request shouldn't be cached */

    p = rtu->p;
    slave = 1; // q->buf[6];
    addr = 0; //q->buf[8] >> 8 | q->buf[9];
    nb = 4; // q->buf[10] >> 8 | q->buf[11];
    while (p) {
        if (addr == p->addr && slave == p->slaveid && nb == p->len)
            return p;
        p = p->next;
    }

    return p;
}

int queue_add(int slave_id, int fd, const u_int8_t *buf, size_t len)
{
    struct slave_map *mi;
    struct rtu_desc *ri;
    struct queue_list q;

    pthread_rwlock_wrlock(&rwlock);

    VFOREACH(rtu_list, ri) {
        VFOREACH(ri->slave_id, mi) {
            if (mi->src == slave_id)
                goto found;
        }
    }

    pthread_rwlock_unlock(&rwlock);
    return -2;

found:
    q.stamp = 0;
    q.resp_fd = fd;
    q.buf = malloc(len);
    q.len = len;
    /* TODO: fixup slave_id address */
    memcpy(q.buf, buf, len);

    QADD(ri->q, q);

    pthread_rwlock_unlock(&rwlock);

    return 0;
}

void _queue_pop(struct rtu_desc *rtu)
{
    struct queue_list *q;

    if (!rtu)
        return;

    q = &QREMOVE(rtu->q);
    close(q->resp_fd);
    free(q->buf);
    q->buf = NULL;
}

int setnonblocking(int sockfd)
{
    int opts;
    opts = fcntl(sockfd, F_GETFL);
    if (opts < 0)
        return errno;
    opts = (opts | O_NONBLOCK);
    if (fcntl(sockfd, F_SETFL, opts) < 0)
        return errno;

    return 0;
}

int rtu_open_serial(struct rtu_desc *rtu)
{
    /* TODO: setup baud rate, bits and parity */
    return open(rtu->cfg.serial.devname, O_RDWR | O_NONBLOCK);
}

int rtu_open_tcp(struct rtu_desc *rtu)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    char service[NI_MAXSERV];

    sprintf(service, "%d", 502);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* Datagram socket */
    hints.ai_flags = AI_NUMERICSERV;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(rtu->cfg.hostname, service, &hints, &result) != 0) {
        return -1;
    }

    rtu->fd = -1;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        int s;

        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0)
            continue;
 
        if (rp->ai_family == AF_INET) {
            int opt = 1;
            /* Set the TCP no delay flag */
            if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                           (const void *)&opt, sizeof(int)) == -1) {
                close(s);
                continue;
            }

            opt = IPTOS_LOWDELAY;
            if (setsockopt(s, IPPROTO_IP, IP_TOS,
                           (const void *)&opt, sizeof(int)) == -1) {
                close(s);
                continue;
            }

            if (connect(s, rp->ai_addr, rp->ai_addrlen) != 0) {
                close(s);
                continue;
            }

            rtu->fd = s;
            break;
        }
    }
    freeaddrinfo(result);

    return rtu->fd;
}

int rtu_open(struct rtu_desc *rtu, int ep)
{
    int rc = -1;
    struct epoll_event ev;

    switch (rtu->type) {
    case RTU:
    case ASCII:
        rc = rtu_open_serial(rtu);
        if (rc < 0) {
            printf("Unable to open %s (%d)\n", rtu->cfg.serial.devname, errno);
        }
        break;
    
    case TCP:
        rc = rtu_open_tcp(rtu);
        if (rc < 0) {
            printf("Unable to connect to %s (%d)\n", rtu->cfg.hostname, errno);
        }
        break;
    }

    if (rc) {
        ev.data.fd = rc;
        ev.events = EPOLLIN | EPOLLERR;

        if (epoll_ctl(ep, EPOLL_CTL_ADD, ev.data.fd, &ev) == -1) {
            perror("epoll_ctl(rtu) failed");
            close(rc);
            rc = -1;
        }
    }

    rtu->fd = rc;
    return rc;
}

void rtu_close(struct rtu_desc *rtu, int ep)
{
    epoll_ctl(ep, EPOLL_CTL_DEL, rtu->fd, NULL);
    close(rtu->fd);
    rtu->fd = -1;
}

void *rtu_thread(void *arg)
{
    int ep;
    struct rtu_desc r;
    struct rtu_desc *ri;
    struct slave_map m;
    struct cache_page *p;
    queue_list_v *qv;
    struct queue_list *q;
    u_int8_t buf[BUF_SIZE];
    struct epoll_event *evs;

    /* TODO: Read config first */
    VINIT(rtu_list);

    /* :HACK: */
    m.src = 1;
    m.dst = 247;
    r.type = TCP;
    r.cfg.hostname = strdup("37.140.168.193");
    VINIT(r.slave_id);
    QINIT(r.q);
    VADD(r.slave_id, m);
    VADD(rtu_list, r);

    m.src = 2;
    m.dst = 247;
    r.type = TCP;
    r.cfg.hostname = strdup("37.140.168.194");
    VINIT(r.slave_id);
    QINIT(r.q);
    VADD(r.slave_id, m);
    VADD(rtu_list, r);
    /* :HACK: */

    ep = epoll_create(VLEN(rtu_list));
    if (ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    evs = malloc(sizeof(struct epoll_event) * VLEN(rtu_list));

    /* TODO: Proceed to open all RTU */
    VFOREACH(rtu_list, ri) {
        rtu_open(ri, ep);
    }

    for (;;) {
        int n;
        time_t cur_time;
        int nfds = epoll_wait(ep, evs, VLEN(rtu_list), 500);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait(rtu) failed");
            goto err;
        }

        /* Process RTU data */
        for (n = 0; n < nfds; ++n) {
            int len;

            if (!(evs[n].events & EPOLLIN))
                continue;

            /* Get RTU by descriptor */
            ri = rtu_by_fd(evs[n].data.fd);
            if (!ri) {
                /* Should never happens, just clear the event */
                len = read(evs[n].data.fd, buf, BUF_SIZE);
                rtu_close(ri, ep);
                continue;
            }

            len = read(evs[n].data.fd, buf, BUF_SIZE);
            if (len <= 0) {
                /* Re-open required */
                rtu_close(ri, ep);
                rtu_open(ri, ep);
                fprintf(stderr, "Read failed, trying to re-open %d\n", ri->fd);
                continue;
            }

            /* Update cache */
            cache_update(ri, buf, len);
        }

        pthread_rwlock_wrlock(&rwlock);
        cur_time = time(NULL);
        VFOREACH(rtu_list, ri) {
            if (!ri->fd) {
                /* TODO: handle failed RTUs (number of attemps) and 
                 *       answer to query for failed RTUs
                 */
                rtu_open(ri, ep);
                continue;
            }

            /* Invalidate cache pages */
            p = ri->p;
            while (p) {
                if (p->ttd && p->ttd <= cur_time) {
                    p = _page_free(ri, p);
                    continue;
                }
                p = p->next;
            }

            /* Process queue */
            qv = &ri->q;
            for (n = 0; n < QLEN(*qv); ++n) {
                /* Check for timeouted items */
                q = &QGET(*qv, n);
                if (q->stamp && q->stamp <= cur_time) {
                    // build response with TIMEOUT error message
                    write(q->resp_fd, "\x00\x01\x00", 3);
                    _queue_pop(ri);
                    n--;
                    continue;
                } else if (q->stamp) {
                    break;
                } else {
                    /* Check for cache page */
                    p = _cache_find(ri, q);
                    if (p) {
                        write(q->resp_fd, p->buf, p->len);
                        _queue_pop(ri);
                        n--;
                        continue;
                    }

                    /* Make request to RTU */
                    if (write(ri->fd, q->buf, q->len) != q->len) {
                        perror("write() failed");
                    }

                    q->stamp = time(NULL) + RTU_TIMEOUT;
                    break;
                }
            }
//            printf("slave_id=%d queue=%d\n", ri->slave_id, i);
        }
        pthread_rwlock_unlock(&rwlock);
    }

err:
    return NULL;
}

void *tcp_thread(void *p)
{
    int n;
    u_int8_t buf[BUF_SIZE];
    struct childs *self = (struct childs *)p;
    struct epoll_event evs[MAX_EVENTS];

    self->ep = epoll_create(MAX_EVENTS);
    if (self->ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    /* Main loop */
    for (;;) {
        int nfds = epoll_wait(self->ep, evs, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait(tcp) failed");
            goto err;
        }

        for (n = 0; n < nfds; ++n) {
            int len;
            if (!(evs[n].events & EPOLLIN))
                continue;

            len = read(evs[n].data.fd, buf, BUF_SIZE);
            if (len == 0) {
c_close:
                epoll_ctl(self->ep, EPOLL_CTL_DEL, evs[n].data.fd, NULL);
                close(evs[n].data.fd);
            } else if (len < 0) {
                perror("Error occured");
            } else {
                char ans[] = "HTTP/1.1 200 OK\r\nServer: fake/1.0\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html>OK</html>\r\n";
                int sz = sizeof(ans);
                write(evs[n].data.fd, ans, sz);

                queue_add(1 /* buf[6] */, evs[n].data.fd, buf, len);

                // fprintf(stderr, "%d Read %d bytes from %d\n", self->n, len, evs[n].data.fd);
                goto c_close;
            }
        } 
    }

err: 
    close(self->ep);

    return NULL;
}

int main(int argc, char **argv)
{
    int c;
    int n;
    int sd;
    int ep;
    int cur_child = 0;
    pthread_t rtu_proc;
    pthread_attr_t attr;
    struct sockaddr_in6 sin6;
    struct epoll_event ev;
    struct epoll_event evs[1];
    static struct childs childs[CHILD_NUM+1];

    if ((sd = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        perror("socket() failed");
        return 1;
    }

    n = 1;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
        perror("setsockopt() failed");
        return 1;
    }

    memset(&sin6, 0, sizeof(sin6));

    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(MODBUS_TCP_PORT);
    sin6.sin6_addr = in6addr_any;

    /* Bind to IPv6 */
    if (bind(sd, (struct sockaddr *)&sin6, sizeof(sin6)) < 0) {
        perror("bind() failed");
        return 1;
    }
    if (listen(sd, MAX_EVENTS >> 1) < 0) {
        perror("listen() failed");
        return 1;
    }

#if 0
      /********************************************************************/
      /* In this example we know that the client will send 250 bytes of   */
      /* data over.  Knowing this, we can use the SO_RCVLOWAT socket      */
      /* option and specify that we don't want our recv() to wake up      */
      /* until all 250 bytes of data have arrived.                        */
      /********************************************************************/
      if (setsockopt(sdconn, SOL_SOCKET, SO_RCVLOWAT,
                     (char *)&rcdsize,sizeof(rcdsize)) < 0)
      {
         perror("setsockopt(SO_RCVLOWAT) failed");
         break;
      }
#endif

    ep = epoll_create(1);
    if (ep == -1) {
        perror("epoll_create() failed");
        return 1;
    }

    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = sd;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, sd, &ev) == -1) {
        perror("epoll_ctl(sd) failed");
        return 1;
    }

    /* Pre-fork threads */
    pthread_rwlock_init(&rwlock, NULL);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&rtu_proc, &attr, rtu_thread, NULL) < 0) {
        perror("pthread_create() failed");
        return 2;
    }

    for (n = 0; n < CHILD_NUM; ++n) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        childs[n].n = n;
        if (pthread_create(&childs[n].th, &attr, tcp_thread, &childs[n]) < 0) {
            perror("pthread_create() failed");
            return 2;
        }
    }

    for (;;) {
        struct sockaddr_in6 local;
        socklen_t addrlen = sizeof(local);
        int nfds;

        nfds = epoll_wait(ep, evs, 1, -1);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;            
            perror("epoll_wait(main) failed");
            return 2;
        } else if (nfds == 0) {
            continue;
        }

        if (!(evs[0].events & EPOLLIN))
            continue;

//        fprintf(stderr, "%d events=%d\n", nfds, evs[0].events);

        c = accept(sd, (struct sockaddr *)&local, &addrlen);

        if (setnonblocking(c) < 0) {
            perror("setnonblocking()");
            close(c);
        } else {
            ev.events = EPOLLIN;
            ev.data.fd = c;
//            fprintf(stderr, "%d Adding(%d) %d %d\n", ep, i, c, ((struct sockaddr_in6 *)&local)->sin6_port);
            if (epoll_ctl(childs[cur_child++].ep, EPOLL_CTL_ADD, c, &ev) < 0) {
                perror("epoll_ctl ADD()");
                close(c);
            }
            cur_child %= CHILD_NUM;
        }
    }

    pthread_rwlock_destroy(&rwlock);
    close(sd);

    return 0;
}
