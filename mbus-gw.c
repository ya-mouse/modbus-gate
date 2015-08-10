#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#ifndef _NUTTX_BUILD
#include <netinet/tcp.h>
#include <netinet/in.h>
#endif
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

#include "mbus-gw.h"
#ifndef _NUTTX_BUILD
#include "aspp.h"
#endif
#include "cfg.h"
#include "rtu.h"

#ifdef _NUTTX_BUILD
#define pthread_rwlock_t          pthread_mutex_t
#define pthread_rwlock_init(x, y) pthread_mutex_init(x, y)
#define pthread_rwlock_destroy(x) pthread_mutex_destroy(x)
#define pthread_rwlock_rdlock(x)  pthread_mutex_lock(x)
#define pthread_rwlock_wrlock(x)  pthread_mutex_lock(x)
#define pthread_rwlock_unlock(x)  pthread_mutex_unlock(x)
#endif

#undef MAX_EVENTS
#define MAX_EVENTS 3

static pthread_rwlock_t rwlock;

struct rtu_desc *rtu_by_slaveid(struct cfg *cfg, int slave_id)
{
    struct slave_map *mi;
    struct rtu_desc *ri;
    int rc;

    if ((rc = pthread_rwlock_rdlock(&rwlock)) != 0) {
        printf("rtu_by_slaveid: rdlock=%d\n", rc);
        return NULL;
    }
    VFOREACH(cfg->rtu_list, ri) {
        VFOREACH(ri->slave_id, mi) {
            if (mi->src == slave_id)
                goto out;
        }
    }
    ri = NULL;

out:
    if (pthread_rwlock_unlock(&rwlock) != 0)
        printf("rtu_by_slaveid: unlock FAILED\n");

    return ri;
}

struct rtu_desc *rtu_by_fd(struct cfg *cfg, int fd)
{
    struct rtu_desc *ri;
    int rc;

    if ((rc = pthread_rwlock_rdlock(&rwlock)) != 0) {
        printf("rtu_by_fd: rdlock=%d\n", rc);
        return NULL;
    }
    VFOREACH(cfg->rtu_list, ri) {
        if (ri->fd == fd
#ifndef _NUTTX_BUILD
            || (ri->type == REALCOM && ri->cfg.realcom.cmdfd == fd)
#endif
           )
            goto out;
    }
    ri = NULL;

out:
    if (pthread_rwlock_unlock(&rwlock) != 0)
        printf("rtu_by_fd: unlock FAILED\n");

    return ri;
}

void cache_update(struct rtu_desc *rtu, const uint8_t *buf, size_t len)
{
    int slave = 0;
    int func = 0;
    int addr = 0;
    int nb = 0;
    int i;
    int rc;
    struct queue_list *q;
    struct cache_page *p;
    struct cache_page *new;

    if (!rtu || !QLEN(rtu->q)) {
        DEBUGF("rtu=%p qlen=%d\n", rtu, (rtu ? QLEN(rtu->q) : -1));
        return;
    }

    if ((rc = pthread_rwlock_wrlock(&rwlock)) != 0) {
        printf("cache_update: wrlock=%d\n", rc);
        return;
    }
    /* TODO: Write (change register) request shouldn't be cached */
    /* Check for function type */
    // ...

    q = &QGET(rtu->q, 0);
    /* TODO: handle different RTU types */
    if (rtu->type == TCP) {
        slave = q->buf[6];
        func = q->buf[7];
        addr = (q->buf[8] << 8) | q->buf[9];
        nb = (q->buf[10] << 8) | q->buf[11];
    } else if (rtu->type == RTU) {
        slave = q->buf[0];
        func = q->buf[1];
        addr = (q->buf[2] << 8) | q->buf[3];
        nb = (q->buf[4] << 8) | q->buf[5];
    }

#ifdef DEBUG
    printf("-- ");
    for (i=0; i<q->len; i++) {
        printf("%02x ", q->buf[i]);
    }
    printf("\n++ ");
    for (i=0; i<len; i++) {
        printf("%02x ", buf[i]);
    }
    printf("\n");

    printf("update sid=%d addr=%d nb=%d for %d %d\n",
           slave, addr, nb, buf[6], buf[8] >> 1);
#endif

    /* TODO: Check for Query TID and Response TID */
    if (rtu->type == TCP) {
        if (q->buf[0] != buf[0] || q->buf[1] != buf[1]) {
            DEBUGF(">>> Wrong ordered response: %d expected %d\n",
                   (buf[0] << 8) | buf[1], (q->buf[0] << 8) | q->buf[1]);
            if (pthread_rwlock_unlock(&rwlock) != 0)
                printf("cache_update: 0 unlock FAILED\n");
            return;
        }
    }

    /* For error response do not cache the answer, just write it back */
//    if (buf[7] & 0x80)

    if (!rtu->p) {
        p = rtu->p = calloc(1, sizeof(struct cache_page));
        p->next = NULL;
        p->prev = NULL;
    } else {
        p = rtu->p;
        while (p->next != NULL) {
            if (func == p->function && slave == p->slaveid && addr == p->addr) {
                /* Update page if it has the same constraints */
//                printf("update %p (%d,%d)\n", p, nb, p->len);
                if (nb == ((buf[10] << 8) | buf[11])) //p->len)
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
    }

fill:
    p->function = func;
    p->slaveid = slave;
    p->addr = addr;
    p->buf = malloc(len);
    p->len = len;

update:
    if (rtu->type == TCP) {
        memcpy(p->buf, buf, len);
    } else if (rtu->type == RTU) {
        /* Remove CRC */
        memcpy(p->buf, buf, len-2);
    }
    /* TODO: TTL have to be configured via config for each RTU / slave */
//    q->stamp = 0;
    p->ttd = time(NULL) + rtu->conf->ttl;

    if (pthread_rwlock_unlock(&rwlock) != 0)
        printf("cache_update: unlock FAILED\n");
}

inline struct cache_page *_page_free(struct rtu_desc *rtu, struct cache_page *p)
{
    struct cache_page *next;

    if (!rtu || !p)
        return NULL;

    free(p->buf);
    if (!p->prev) {
        rtu->p = p->next;
        if (rtu->p)
            rtu->p->prev = NULL;
    } else {
        p->prev->next = p->next;
        if (p->next)
            p->next->prev = p->prev;
    }
    next = p->next;
    free(p);

    return next;
}

struct cache_page *_cache_find(struct rtu_desc *rtu, struct queue_list *q)
{
    int slave = 0;
    int addr = 0;
    int func = 0;
    int nb = 0;
    struct cache_page *p;

    if (!rtu)
        return NULL;

    /* TODO: Write (change register) request shouldn't be cached */

    p = rtu->p;
    if (rtu->type == TCP) {
        slave = q->buf[6];
        func = q->buf[7];
        addr = (q->buf[8] << 8) | q->buf[9];
        nb = (q->buf[10] << 8) | q->buf[11];
    } else if (rtu->type == RTU) {
        slave = q->buf[0];
        func = q->buf[1];
        addr = (q->buf[2] << 8) | q->buf[3];
        nb = (q->buf[4] << 8) | q->buf[5];
    }

    //DEBUGF("search for sid=%d addr=%d nb=%d\n", slave, addr, nb);

    while (p) {
        DEBUGF("--> (%d,%d,%d) <> (%d,%d,%d)\n",
               slave, func, addr, p->slaveid, p->function, p->addr);
        if (func == p->function && slave == p->slaveid && addr == p->addr)
            return p;
        p = p->next;
    }

    return p;
}

static void dump(const uint8_t *buf, size_t len)
{
#ifdef DEBUG
    int i;
    for (i=1; i<=len; ++i) {
        printf("%02x ", buf[i-1]);
        if (!(i % 16))
            printf("\n");
    }
    if ((i % 16))
        printf("\n");
#endif
}

int queue_add(struct cfg *cfg,
              int slave_id, int fd, const uint8_t *buf, size_t len)
{
    struct slave_map *mi;
    struct rtu_desc *ri;
    struct queue_list q;
    int rc;

    if ((rc = pthread_rwlock_wrlock(&rwlock)) != 0) {
        printf("queue_add: wrlock=%d\n", rc);
        return -1;
    }

    VFOREACH(cfg->rtu_list, ri) {
        VFOREACH(ri->slave_id, mi) {
            if (mi->src == slave_id)
                goto found;
        }
    }

    if (pthread_rwlock_unlock(&rwlock) != 0)
        printf("queue_add: 0 unlock FAILED\n");
    return -2;

found:
    DEBUGF("Adding sid=%d to queue len=%d fd=#%d\n", slave_id, len, fd);

    q.stamp = 0;
    q.resp_fd = fd;
    DEBUGF("=== orig === %d\n", fd);
    dump(buf, len);
    DEBUGF("=== added === %d\n", fd);
    if (ri->type == RTU) {
        uint16_t crc;
        q.buf = malloc(len-4);
        q.len = len-4;
        q.tido[0] = buf[0];
        q.tido[1] = buf[1];
        memcpy(q.buf, buf+6, len-6);
        q.buf[0] = mi->dst;
        q.src = mi->src;
        crc = crc16(q.buf, len-6);
        memcpy(q.buf+q.len-2, &crc, 2);
        dump(q.buf, q.len);
    } else {
        q.buf = malloc(len);
        q.len = len;
        q.src = mi->src;
        memcpy(q.buf, buf, len);
        /* Fixup destination slave address */
        q.buf[6] = mi->dst;
    }

    QADD(ri->q, q);

    if (pthread_rwlock_unlock(&rwlock) != 0)
        printf("queue_add: 0 unlock FAILED\n");

    return 0;
}

inline void _queue_pop(struct rtu_desc *rtu)
{
    struct queue_list *q;

    if (!rtu)
        return;

    q = &QREMOVE(rtu->q);
    free(q->buf);
    q->buf = NULL;
}

void *rtu_thread(void *arg)
{
    int ep;
    int rc;
    struct rtu_desc *ri;
    struct cache_page *p;
    queue_list_v *qv;
    struct queue_list *q;
    struct epoll_event *evs;
    struct cfg *cfg = (struct cfg *)arg;

    ep = epoll_create(VLEN(cfg->rtu_list));
    if (ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    evs = malloc(sizeof(struct epoll_event) * VLEN(cfg->rtu_list));

    VFOREACH(cfg->rtu_list, ri) {
        ri->conf = cfg;
        rtu_open(ri, ep);
    }

    fprintf(stderr, "RTU Ready: %08x %d\n", ep, VLEN(cfg->rtu_list));

    for (;;) {
        int n;
        time_t cur_time;
        int nfds = epoll_wait(ep, evs, VLEN(cfg->rtu_list), 10);
        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait(rtu) failed");
            goto err;
        }

#if 0
    if (getsockopt (socketFD, SOL_SOCKET, SO_ERROR, &retVal, &retValLen) < 0)
    {
       // ERROR, fail somehow, close socket
    }

    if (retVal != 0) 
    {
       // ERROR: connect did not "go through"
    }
#endif

        /* Process RTU data */
        for (n = 0; n < nfds; ++n) {
            int len;

            if (!(evs[n].events & EPOLLIN)) {
                if (evs[n].events & EPOLLHUP) {
                    goto reconnect;
                } else if (evs[n].events & EPOLLOUT) {
//                    epoll_ctl(self->ep, EPOLL_CTL_DEL, evs[n].data.fd, NULL);
//                    printf("ev=%x\n", evs[n].events);
                }
                continue;
            }

            /* Get RTU by descriptor */
            ri = rtu_by_fd(cfg, evs[n].data.fd);
            if (!ri) {
                /* Should never happens, just clear the event */
//                len = read(evs[n].data.fd, buf, BUF_SIZE);
                rtu_close(ri, ep);
                continue;
            }

            len = read(evs[n].data.fd, ri->toreadbuf+ri->toread_off, ri->toread);
            if (len <= 0) {
reconnect:
                /* TODO: reset "retries" counters after a delay */
                /* Re-open required */
                fprintf(stderr, "Read failed (%d), trying to re-open #%d\n",
                        errno, ri->fd);
                rtu_close(ri, ep);
                rtu_open(ri, ep);
                continue;
            }
            DEBUGF(">>> Read %d bytes from #%d\n", len, evs[n].data.fd);
            dump(ri->toreadbuf, ri->toread_off + len);

#ifndef _NUTTX_BUILD
            /* Process RealCOM command */
            if (ri->type == REALCOM && ri->fd != evs[n].data.fd) {
                DEBUGF("Process CMD %d (#%d,#%d)\n", len, ri->fd, evs[n].data.fd);
                realcom_process_cmd(ri, ri->toreadbuf, len);
                continue;
            }
#endif

            if (ri->type == RTU) {
                if ((ri->toreadbuf[1] & 0xf0) == 0x80) {
                    DEBUGF("...exception(#%d): %02x\n", ri->fd, ri->toreadbuf[1]);
                    ri->toread = 0;
                    ri->toread_off += len;
                } else {
                    ri->toread -= len;
                    ri->toread_off += len;
                }
                if (ri->toread > 0) {
                    DEBUGF("...more(#%d): %d\n", ri->fd, ri->toread);
                    continue;
                }

                /* Update last serial activity timestamp */
                gettimeofday(&ri->tv, NULL);
            }

            /* Update cache */
            cache_update(ri, ri->toreadbuf, ri->toread_off);
        }

        if ((rc = pthread_rwlock_wrlock(&rwlock)) != 0) {
            printf("rtu_thread: wrlock=%d\n", rc);
            continue;
        }

        cur_time = time(NULL);
        VFOREACH(cfg->rtu_list, ri) {
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
                //DEBUGF("-- cache: %lu <> %lu\n", p->ttd, cur_time);
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
                    DEBUGF("Remove from queue: %d %ld %ld\n", q->buf[6], q->stamp, cur_time);
                    if (q->resp_fd >= 0)
                        write(q->resp_fd, "\x00\x01\x00\x00\x00\x02\x01\x83", 8);
                    ri->toread = 0;
                    ri->toread_off = 0;
                    if (ri->toreadbuf)
                        free(ri->toreadbuf);
                    ri->toreadbuf = NULL;
                    _queue_pop(ri);
                    n--;
                    continue;
                } else {
                    /* Check for cache page */
                    p = _cache_find(ri, q);
                    if (p) {
                        DEBUGF("Found, respond to #%d len=%d\n",
                               q->resp_fd, p->len);
                        if (ri->type == TCP) {
#if 1
                            /* Revert TID back */
                            p->buf[0] = ri->tido[0];
                            p->buf[1] = ri->tido[1];
#endif
                            if (q->resp_fd >= 0)
                                write(q->resp_fd, p->buf, p->len);
                        } else if (ri->type == RTU) {
                            uint8_t tcp[520];
                            tcp[0] = q->tido[0];
                            tcp[1] = q->tido[1];
                            tcp[2] = tcp[3] = 0;
                            tcp[4] = ((p->len-2) >> 8) & 0xff;
                            tcp[5] = (p->len-2) & 0xff;
                            tcp[6] = q->src;
                            memcpy(tcp+7, p->buf+1, p->len-3);
                            if (q->resp_fd >= 0)
                                write(q->resp_fd, tcp, p->len + 4);
                            dump(tcp, p->len + 4);
                        }
                        _queue_pop(ri);
                        n--;
                        continue;
                    } else if (q->stamp) {
                        break;
                    }

                    /* RTU Endpoint is alive */
                    if (ri->fd >= 0) {

                    if (ri->type == TCP) {
#if 1
                        /* Fixup TID */
                        ri->tido[0] = q->buf[0];
                        ri->tido[1] = q->buf[1];
                        q->buf[0] = ri->tid >> 8;
                        q->buf[1] = ri->tid & 0xff;
                        ri->tid++;
                        /* Zero is not allowed as TID */
                        if (!ri->tid)
                            ri->tid ^= 1;
#endif
                        /* Make request to RTU */
                        if (write(ri->fd, q->buf, q->len) != q->len) {
                            perror("write() failed");
                        }
                    } else if (ri->type == RTU) {
                        struct timeval tv;
                        gettimeofday(&tv, NULL);

                        if (ri->toread <= 0 && ((tv.tv_sec - ri->tv.tv_sec) > 0 || (tv.tv_usec - ri->tv.tv_usec) > 200000)) {
                            write(ri->fd, q->buf, q->len);
                            // status register
                            if (q->buf[1] == 1 || q->buf[1] == 2) {
                                ri->toread = 6;
                            } else {
                                ri->toread = ((q->buf[4] << 8) | q->buf[5]) * 2 + 5;
                            }
                            ri->toreadbuf = malloc(ri->toread);
                            ri->toread_off = 0;
                            DEBUGF("toread(#%d): %d\n", ri->fd, ri->toread);
                        } else {
                            DEBUGF("toread(#%d)==%d\n", ri->fd, ri->toread);
                            continue;
                        }
                    }
                    DEBUGF("Write to RTU: #%d l=%d\n", ri->fd, q->len);

                    }
                    q->stamp = time(NULL) + ri->timeout;
                    break;
                }
            }
        }

        if (pthread_rwlock_unlock(&rwlock) != 0)
            printf("rtu_thread: unlock FAILED\n");
    }

err:
    return NULL;
}

void *tcp_thread(void *p)
{
    int n;
    uint8_t buf[BUF_SIZE];
    struct workers *self = (struct workers *)p;
    struct epoll_event evs[MAX_EVENTS];

    self->ep = epoll_create(MAX_EVENTS);
    if (self->ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    /* Main loop */
    for (;;) {
#ifdef _NUTTX_BUILD
        int nfds = epoll_wait(self->ep, evs, MAX_EVENTS, 100);
#else
        int nfds = epoll_wait(self->ep, evs, MAX_EVENTS, -1);
#endif

        if (nfds == -1) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait(tcp) failed");
            continue;
//            goto err;
        }

        for (n = 0; n < nfds; ++n) {
            int len;
            if (!(evs[n].events & EPOLLIN)) {
                if (evs[n].events & EPOLLHUP) {
                    /* TODO: dealloc RTU by fd */
                    close(evs[n].data.fd);
                }
                continue;
            }

            len = read(evs[n].data.fd, buf, BUF_SIZE);
            if (len == 0) {
//c_close:
                epoll_ctl(self->ep, EPOLL_CTL_DEL, evs[n].data.fd, NULL);
                close(evs[n].data.fd);
            } else if (len < 0) {
                perror("Error occured");
            } else {
                queue_add(self->cfg, buf[6], evs[n].data.fd, buf, len);
            }
        } 
    }

err:
#ifdef _NUTTX_BUILD
    epoll_close(self->ep);
#else
    close(self->ep);
#endif

    return NULL;
}

#ifdef _NUTTX_BUILD
# ifdef CONFIG_BUILD_KERNEL
int main(int argc, FAR char *argv[])
# else
int mbusgw_main(int argc, FAR char *argv[])
# endif
#else
int main(int argc, char *argv[])
#endif
{
    int c;
    int n;
    int rc;
    int sd;
    int ud;
    int ep;
    int cur_child = 0;
    pthread_t rtu_proc;
    pthread_attr_t attr;
    struct sockaddr_un name;
#ifndef _NUTTX_BUILD
    struct sockaddr_in6 sin6;
#else
    struct sockaddr_in sin;
#endif
    struct epoll_event ev;
    struct epoll_event evs[2];
    struct cfg *cfg;
    static struct workers *workers;

    cfg = cfg_load("mbus.conf");
    if (!cfg) {
        return 1;
    }

    if (unlink(cfg->sockfile) < 0 && errno != ENOENT) {
        perror("unlink(sockfile) failed");
        return 1;
    }

#ifndef _NUTTX_BUILD
    if ((sd = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
#else
    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
#endif
        perror("socket(AF_INET6) failed");
        return 1;
    }

#ifndef _NUTTX_BUILD
    if ((ud = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        perror("socket(PF_LOCAL) failed");
        return 1;
    }
#endif

    n = 1;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
        perror("setsockopt() failed");
        return 1;
    }

    memset(&name, 0, sizeof(name));
#ifndef _NUTTX_BUILD
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(MODBUS_TCP_PORT);
    sin6.sin6_addr = in6addr_any;
#else
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(MODBUS_TCP_PORT);
    sin.sin_addr.s_addr = htonl(0x0a000002);
#endif
    name.sun_family = AF_LOCAL;
    strcpy(name.sun_path, cfg->sockfile);

#ifndef _NUTTX_BUILD
    /* Bind to IPv6 */
    if (bind(sd, (struct sockaddr *)&sin6, sizeof(sin6)) < 0) {
        perror("bind(sd) failed");
        return 1;
    }
#else
    /* Bind to IPv4 */
    if (bind(sd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind(sd) failed");
        return 1;
    }
#endif
    if (listen(sd, MAX_EVENTS >> 1) < 0) {
        perror("listen(sd) failed");
        return 1;
    }

#ifndef _NUTTX_BUILD
    /* Bind to UNIX socket */
    if (bind(ud, (struct sockaddr *)&name, SUN_LEN(&name)) < 0) {
        perror("bind(ud) failed");
        return 1;
    }
    if (listen(ud, MAX_EVENTS >> 1) < 0) {
        perror("listen(ud) failed");
        return 1;
    }
#endif

    ep = epoll_create(2);
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

#ifndef _NUTTX_BUILD
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = ud;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, ud, &ev) == -1) {
        perror("epoll_ctl(ud) failed");
        return 1;
    }
#endif

    /* Pre-fork threads */
    pthread_rwlock_init(&rwlock, NULL);

    pthread_attr_init(&attr);
#ifdef PTHREAD_CREATE_DETACHED
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#endif
    if (pthread_create(&rtu_proc, &attr, rtu_thread, cfg) < 0) {
        perror("pthread_create() failed");
        return 2;
    }
#ifndef PTHREAD_CREATE_DETACHED
    pthread_detach(rtu_proc);
#endif

    workers = malloc(sizeof(struct workers) * cfg->workers);

    for (n = 0; n < cfg->workers; ++n) {
        pthread_attr_init(&attr);
#ifdef PTHREAD_CREATE_DETACHED
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#endif
        workers[n].n = n;
        workers[n].cfg = cfg;
        if (pthread_create(&workers[n].th, &attr, tcp_thread, &workers[n]) < 0) {
            perror("pthread_create() failed");
            return 2;
        }
#ifndef PTHREAD_CREATE_DETACHED
        pthread_detach(workers[n].th);
#endif
    }

    for (;;) {
#ifndef _NUTTX_BUILD
        struct sockaddr_in6 local;
#else
        struct sockaddr_in local;
#endif
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

        for (n = 0; n < nfds; ++n) {
            if (!(evs[n].events & EPOLLIN))
                continue;

//            fprintf(stderr, "%d events=%d\n", nfds, evs[0].events);

            c = accept(evs[n].data.fd, (struct sockaddr *)&local, &addrlen);

            if (c < 0) {
                perror("accept()");
                /* HACK: unable to accept more incoming connections */
                rc = -1;
                goto die;
            }

            if (setnonblocking(c) < 0) {
                perror("setnonblocking()");
                close(c);
            } else {
                ev.events = EPOLLIN;
                ev.data.fd = c;
//                fprintf(stderr, "%d Adding() %d %d\n", evs[n].data.fd, c, ((struct sockaddr_in *)&local)->sin_port);
//                fprintf(stderr, "%d Adding() %d %d\n", ep, c, ((struct sockaddr_in6 *)&local)->sin6_port);
                if (epoll_ctl(workers[cur_child++].ep, EPOLL_CTL_ADD, c, &ev) < 0) {
                    perror("epoll_ctl ADD()");
                    close(c);
                }
                cur_child %= cfg->workers;
            }
        }
    }

    rc = 0;
die:
    pthread_rwlock_destroy(&rwlock);
#ifndef _NUTTX_BUILD
    close(ud);
#endif
    close(sd);

    return 0;
}
