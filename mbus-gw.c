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
#include <pthread.h>
#include <time.h>

#include "vect.h"

/* Slave address (1-255 for RTU/ASCII, 0-255 for TCP) */

#define CHILD_NUM       4
#define BUF_SIZE        512
#define MAX_EVENTS      1024
#define MODBUS_TCP_PORT 502
#define RTU_TIMEOUT     3
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
    int status;         /* 0 - ok, 1 - timeout, 2 - NA */
    int slaveid;
    int addr;
    int function;
    int len;
    time_t ttd;         /* time to die of the page: last_timestamp + TTL */
    u_int8_t *buf;
    struct cache_page *next;
    struct cache_page *prev;
};

struct queue_list {
    int resp_fd;    /* "response to" descriptor */
    u_int8_t *buf;  /* request buffer */
    size_t len;     /* request length */
    time_t stamp;   /* timestamp of timeout: last_timestamp + timeout */
    struct queue_list *next;
    struct queue_list *prev;
};

struct slave_map {
    int src;
    int dst;
};

struct rtu_desc {
    int fd;               /* ttySx descriptior */
    enum rtu_type type;   /* endpoint RTU device type */
    VECT(struct slave_map) slave_id;   /* slave_id configured for MODBUS-TCP */
    struct queue_list *q; /* queue list */
    struct cache_page *p; /* cache pages */
};

struct childs {
    int n;
    int ep;
    pthread_t th;
};

static VECT(struct rtu_desc) rtu;
static pthread_rwlock_t rwlock;

struct rtu_desc *rtu_by_slaveid(int slave_id)
{
    struct slave_map *mi;
    struct rtu_desc *ri;
    struct rtu_desc *r = NULL;

    pthread_rwlock_rdlock(&rwlock);
    VFOREACH(rtu, ri) {
        VFOREACH(ri->slave_id, mi) {
            if (mi->src == slave_id) {
                r = ri;
                goto out;
            }
        }
    }

out:
    pthread_rwlock_unlock(&rwlock);

    return r;
}

struct rtu_desc *rtu_by_fd(int fd)
{
    int i;
    struct rtu_desc *r = NULL;

    pthread_rwlock_rdlock(&rwlock);
    VFORI(rtu, i) {
        if (VGET(rtu, i).fd == fd) {
            r = &VGET(rtu, i);
            break;
        }
    }
    pthread_rwlock_unlock(&rwlock);

    return r;
}

void cache_update(struct rtu_desc *rtu, u_int8_t *buf, size_t len)
{
    int slave = -1;
    int nb = 0;
    int addr = -1;
    struct cache_page *p;
    struct cache_page *new;

    if (!rtu || !rtu->q)
        return;

    pthread_rwlock_wrlock(&rwlock);
    /* Check for function type */
    // ...

    slave = 1; // rtu->q->buf[6];
    addr = 0; //rtu->q->buf[8] >> 8 | rtu->q->buf[9];
    nb = 4; // rtu->q->buf[10] >> 8 | rtu->q->buf[11];

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

    if (!p)
        return NULL;

//    printf("free: %p ", p);
    DBL_FREE(p, next);

//    printf(" n=%p\n", next);
    return next;
}

int queue_add(struct rtu_desc *rtu, int fd, u_int8_t *buf, size_t len)
{
    struct queue_list *q;

    if (!rtu)
        return -1;

    pthread_rwlock_wrlock(&rwlock);
    if (!rtu->q) {
        q = rtu->q = calloc(1, sizeof(struct queue_list));
    } else {
        q = rtu->q;
        while (q->next != NULL)
            q = q->next;
        q->next = malloc(sizeof(struct queue_list));
        q->next->prev = q;
        q = q->next;
    }
    q->stamp = 0;
    q->next = NULL;
    q->resp_fd = fd;
    q->buf = malloc(len);
    q->len = len;
    memcpy(q->buf, buf, len);
    pthread_rwlock_unlock(&rwlock);

    return 0;
}

struct queue_list *_queue_free(struct rtu_desc *rtu, struct queue_list *q)
{
    struct queue_list *next;

    if (!q)
        return NULL;

    close(q->resp_fd);
    DBL_FREE(q, next);

    return next;
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

void *rtu_thread(void *arg)
{
    int ep;
    struct rtu_desc r;
    struct slave_map m;
    struct cache_page *p;
    struct queue_list *q;
    u_int8_t buf[BUF_SIZE];
    struct epoll_event ev;
    struct epoll_event *evs;

    /* TODO: Read config first */
    VINIT(rtu);

    /* :HACK: */ 
    r.fd = dup(0);
    m.src = 1;
    m.dst = 1;
    VINIT(r.slave_id);
    VADD(r.slave_id, m);
    VADD(rtu, r);
    /* :HACK: */

    ep = epoll_create(VLEN(rtu));
    if (ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    evs = malloc(sizeof(struct epoll_event) * VLEN(rtu));

    /* TODO: Proceed to open all RTU */
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = VGET(rtu, 0).fd;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev.data.fd, &ev) == -1) {
        perror("epoll_ctl(0) failed");
        return NULL;
    }

    for (;;) {
        int n;
        time_t cur_time;
        int nfds = epoll_wait(ep, evs, VLEN(rtu), 500);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait(rtu) failed");
            goto err;
        }

        /* Process RTU data */
        for (n = 0; n < nfds; ++n) {
            int len;
            struct rtu_desc *rtu;

            if (!(evs[n].events & EPOLLIN))
                continue;

            /* Get RTU by descriptor */
            rtu = rtu_by_fd(evs[n].data.fd);
            if (!rtu) {
                /* Should never happens, just clear the event */
                len = read(evs[n].data.fd, buf, BUF_SIZE);
                continue;
            }

            len = read(evs[n].data.fd, buf, BUF_SIZE);
            if (len <= 0) {
                /* Re-open required */
                continue;
            }

            /* Update cache */
            cache_update(rtu, buf, len);
        }

        pthread_rwlock_wrlock(&rwlock);
        cur_time = time(NULL);
        VFORI(rtu, n) {
            if (!VGET(rtu, n).fd)
                break;

            /* Invalidate cache pages */
            p = VGET(rtu, n).p;
            while (p) {
                if (p->ttd && p->ttd <= cur_time) {
                    p = _page_free(&VGET(rtu, n), p);
                    continue;
                }
                p = p->next;
            }

            /* Process queue */
            q = VGET(rtu, n).q;
            while (q) {
                /* Check for timeouted items */
                if (q->stamp && q->stamp <= cur_time) {
                    // build response with error message
                    write(q->resp_fd, "\x00\x01\x00", 3);
                    q = _queue_free(&VGET(rtu, n), q);
                    continue;
                } else if (q->stamp) {
                    break;
                } else {
                    int found = 0;
                    /* Write (change) request shouldn't be cached */

                    /* Check for cache page or process new request */
                    if (found) {
                        write(q->resp_fd, "\x00\x01\x00", 3);
                        q = _queue_free(&VGET(rtu ,n), q);
                        continue;
                    }

                    if (write(VGET(rtu, n).fd, q->buf, q->len) != q->len) {
                        perror("write() failed");
                    }

                    q->stamp = time(NULL) + RTU_TIMEOUT;
                    break;
                }
            }
//            printf("slave_id=%d queue=%d\n", VGET(rtu, n).slave_id, i);
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

                queue_add(rtu_by_slaveid(1 /* buf[6] */), evs[n].data.fd, buf, len);

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
