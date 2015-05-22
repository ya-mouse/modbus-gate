#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

#include "mbus-gw.h"
#include "aspp.h"
#include "cfg.h"
#include "rtu.h"

static pthread_rwlock_t rwlock;
static struct cfg *global_cfg;

struct rtu_desc *rtu_by_slaveid(struct cfg *cfg, int slave_id)
{
    struct slave_map *mi;
    struct rtu_desc *ri;

    pthread_rwlock_rdlock(&rwlock);
    VFOREACH(cfg->rtu_list, ri) {
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

struct rtu_desc *rtu_by_fd(struct cfg *cfg, int fd)
{
    struct rtu_desc *ri;

    pthread_rwlock_rdlock(&rwlock);
    VFOREACH(cfg->rtu_list, ri) {
        if (ri->fd == fd ||
           (ri->type == REALCOM && ri->cfg.realcom.cmdfd == fd))
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
    int func;
    int addr;
    int nb;
    int i;
    struct queue_list *q;
    struct cache_page *p;
    struct cache_page *new;

    if (!rtu || !QLEN(rtu->q)) {
        printf("rtu=%p qlen=%d\n", rtu, (rtu ? QLEN(rtu->q) : -1));
        return;
    }

    printf("update_cache\n");
    pthread_rwlock_wrlock(&rwlock);
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
    if (q->buf[0] != buf[0] || q->buf[1] != buf[1]) {
        DEBUGF(">>> Wrong ordered response: %d expected %d\n",
               (buf[0] << 8) | buf[1], (q->buf[0] << 8) | q->buf[1]);
        return;
    }

    /* For error response do not cache the answer, just write it back */
//    if (buf[7] & 0x80)

    if (!rtu->p) {
//        printf("new cache: ");
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
    DEBUGF("alloc %p n=%p p=%p\n", p, p->next, p->prev);

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
    p->ttd = time(NULL) + global_cfg->ttl;
    pthread_rwlock_unlock(&rwlock);
}

inline struct cache_page *_page_free(struct rtu_desc *rtu, struct cache_page *p)
{
    struct cache_page *next;

    if (!rtu || !p)
        return NULL;

//    printf("free: %p ", p);
    free(p->buf);
    if (!p->prev) {
        rtu->p = p->next;
        if (rtu->p)
            rtu->p->prev = NULL;
    } else {
        p->prev->next = p->next;
    }
    next = p->next;
    free(p);

//    printf(" n=%p\n", next);
    return next;
}

struct cache_page *_cache_find(struct rtu_desc *rtu, struct queue_list *q)
{
    int slave;
    int addr;
    int func;
    int nb;
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
               func, slave, addr, p->function, p->slaveid, p->addr);
        if (func == p->function && slave == p->slaveid && addr == p->addr)
            return p;
        p = p->next;
    }

    return p;
}

static void dump(const u_int8_t *buf, size_t len)
{
    int i;
    for (i=1; i<=len; ++i) {
        printf("%02x ", buf[i-1]);
        if (!(i % 16))
            printf("\n");
    }
    if ((i % 16))
        printf("\n");
}

int queue_add(struct cfg *cfg,
              int slave_id, int fd, const u_int8_t *buf, size_t len)
{
    struct slave_map *mi;
    struct rtu_desc *ri;
    struct queue_list q;

    pthread_rwlock_wrlock(&rwlock);

    VFOREACH(cfg->rtu_list, ri) {
        VFOREACH(ri->slave_id, mi) {
            if (mi->src == slave_id)
                goto found;
        }
    }

    pthread_rwlock_unlock(&rwlock);
    return -2;

found:
    DEBUGF("Adding sid=%d to queue len=%d fd=%d\n", slave_id, len, fd);

    q.stamp = 0;
    q.resp_fd = fd;
    if (ri->type == RTU) {
        u_int16_t crc;
        q.buf = malloc(len-4);
        q.len = len-4;
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

    pthread_rwlock_unlock(&rwlock);

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
    struct rtu_desc *ri;
    struct cache_page *p;
    queue_list_v *qv;
    struct queue_list *q;
    u_int8_t buf[BUF_SIZE];
    struct epoll_event *evs;
    struct cfg *cfg = (struct cfg *)arg;

    ep = epoll_create(VLEN(cfg->rtu_list));
    if (ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    evs = malloc(sizeof(struct epoll_event) * VLEN(cfg->rtu_list));

    VFOREACH(cfg->rtu_list, ri) {
        rtu_open(ri, ep);
    }

    fprintf(stderr, "RTU Ready\n");

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
                len = read(evs[n].data.fd, buf, BUF_SIZE);
                rtu_close(ri, ep);
                continue;
            }

            len = read(evs[n].data.fd, buf, BUF_SIZE);
            if (len <= 0) {
reconnect:
                /* TODO: reset "retries" counters after a delay */
                /* Re-open required */
                fprintf(stderr, "Read failed (%d), trying to re-open %d\n",
                        errno, ri->fd);
                rtu_close(ri, ep);
                rtu_open(ri, ep);
                continue;
            }
            DEBUGF(">>> Read %d from #%d\n", len, evs[n].data.fd);
            dump(buf, len);

            /* Process RealCOM command */
            if (ri->type == REALCOM && ri->fd != evs[n].data.fd) {
                printf("Process CMD %d (#%d,#%d)\n", len, ri->fd, evs[n].data.fd);
                realcom_process_cmd(ri, buf, len);
                continue;
            }

            if (ri->type == RTU) {
                ri->toread -= len;
                if (ri->toread > 0) {
                    printf("...more: %d\n", ri->toread);
                    continue;
                }

                /* Update last serial activity timestamp */
                gettimeofday(&ri->tv, NULL);
            }

            /* Update cache */
            cache_update(ri, buf, len);
        }

        pthread_rwlock_wrlock(&rwlock);
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
                    DEBUGF("Remove from queue: %d\n", q->buf[6]);
                    write(q->resp_fd, "\x00\x01\x00\x00\x00\x02\x01\x83", 8);
                    ri->toread = 0;
                    _queue_pop(ri);
                    n--;
                    continue;
                } else {
                    /* Check for cache page */
                    p = _cache_find(ri, q);
                    if (p) {
                        DEBUGF("Found, respond to %d len=%d\n",
                               q->resp_fd, p->len);
                        if (ri->type == TCP) {
#if 1
                            /* Revert TID back */
                            p->buf[0] = ri->tido[0];
                            p->buf[1] = ri->tido[1];
#endif
                            write(q->resp_fd, p->buf, p->len);
                        } else if (ri->type == RTU) {
                            uint8_t tcp[520];
                            tcp[0] = ri->tido[0];
                            tcp[1] = ri->tido[1];
                            tcp[2] = tcp[3] = 0;
                            tcp[4] = (p->len >> 8) & 0xff;
                            tcp[5] = p->len & 0xff;
                            memcpy(tcp+6, p->buf, p->len-2);
                            write(q->resp_fd, tcp, p->len + 4);
                        }
                        _queue_pop(ri);
                        n--;
                        continue;
                    } else if (q->stamp) {
//                        printf("stamp!\n");
                        break;
                    }

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
                            ri->toread = ((q->buf[4] << 8) | q->buf[5]) * 2 + 5;
                            printf("toread: %d\n", ri->toread);
                        } else {
                            printf("toread==%d\n", ri->toread);
                            continue;
                        }
//                        write(ri->fd, q->buf+6, q->len-6);
                    }
                    DEBUGF("Write to RTU: %d l=%d\n", ri->fd, q->len);

                    q->stamp = time(NULL) + ri->timeout;
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
    struct workers *self = (struct workers *)p;
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
//c_close:
                epoll_ctl(self->ep, EPOLL_CTL_DEL, evs[n].data.fd, NULL);
                close(evs[n].data.fd);
            } else if (len < 0) {
                perror("Error occured");
            } else {
#if 0
                char ans[] = "HTTP/1.1 200 OK\r\nServer: fake/1.0\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html>OK</html>\r\n";
                int sz = sizeof(ans);
                write(evs[n].data.fd, ans, sz);
#endif
                queue_add(self->cfg, buf[6], evs[n].data.fd, buf, len);

                // fprintf(stderr, "%d Read %d bytes from %d\n", self->n, len, evs[n].data.fd);
                // goto c_close;
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
    int ud;
    int ep;
    int cur_child = 0;
    pthread_t rtu_proc;
    pthread_attr_t attr;
    struct sockaddr_un name;
    struct sockaddr_in6 sin6;
    struct epoll_event ev;
    struct epoll_event evs[2];
    struct cfg *cfg;
    static struct workers *workers;

    global_cfg = cfg = cfg_load("mbus.conf");
    if (!cfg) {
        return 1;
    }

    if (unlink(cfg->sockfile) < 0 && errno != ENOENT) {
        perror("unlink(sockfile) failed");
        return 1;
    }

    if ((sd = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        perror("socket(AF_INET6) failed");
        return 1;
    }

    if ((ud = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        perror("socket(PF_LOCAL) failed");
        return 1;
    }

    n = 1;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
        perror("setsockopt() failed");
        return 1;
    }

    memset(&sin6, 0, sizeof(sin6));
    memset(&name, 0, sizeof(name));

    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(MODBUS_TCP_PORT);
    sin6.sin6_addr = in6addr_any;

    name.sun_family = AF_LOCAL;
    strcpy(name.sun_path, cfg->sockfile);

    /* Bind to IPv6 */
    if (bind(sd, (struct sockaddr *)&sin6, sizeof(sin6)) < 0) {
        perror("bind(sd) failed");
        return 1;
    }
    if (listen(sd, MAX_EVENTS >> 1) < 0) {
        perror("listen(sd) failed");
        return 1;
    }

    /* Bind to UNIX socket */
    if (bind(ud, (struct sockaddr *)&name, SUN_LEN(&name)) < 0) {
        perror("bind(ud) failed");
        return 1;
    }
    if (listen(ud, MAX_EVENTS >> 1) < 0) {
        perror("listen(ud) failed");
        return 1;
    }

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

    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = ud;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, ud, &ev) == -1) {
        perror("epoll_ctl(ud) failed");
        return 1;
    }

    /* Pre-fork threads */
    pthread_rwlock_init(&rwlock, NULL);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&rtu_proc, &attr, rtu_thread, cfg) < 0) {
        perror("pthread_create() failed");
        return 2;
    }

    workers = malloc(sizeof(struct workers) * cfg->workers);

    for (n = 0; n < cfg->workers; ++n) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        workers[n].n = n;
        workers[n].cfg = cfg;
        if (pthread_create(&workers[n].th, &attr, tcp_thread, &workers[n]) < 0) {
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

        for (n = 0; n < nfds; ++n) {
            if (!(evs[n].events & EPOLLIN))
                continue;

    //        fprintf(stderr, "%d events=%d\n", nfds, evs[0].events);

            c = accept(evs[n].data.fd, (struct sockaddr *)&local, &addrlen);

            if (setnonblocking(c) < 0) {
                perror("setnonblocking()");
                close(c);
            } else {
                ev.events = EPOLLIN;
                ev.data.fd = c;
    //            fprintf(stderr, "%d Adding(%d) %d %d\n", ep, i, c, ((struct sockaddr_in6 *)&local)->sin6_port);
                if (epoll_ctl(workers[cur_child++].ep, EPOLL_CTL_ADD, c, &ev) < 0) {
                    perror("epoll_ctl ADD()");
                    close(c);
                }
                cur_child %= cfg->workers;
            }
        }
    }

    pthread_rwlock_destroy(&rwlock);
    close(ud);
    close(sd);

    return 0;
}
