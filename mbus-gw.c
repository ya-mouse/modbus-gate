#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/wait.h>
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

// rtu_desc_v rtu_list;
static pthread_rwlock_t rwlock;

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

    if (!rtu || !QLEN(rtu->q))
        return;

    pthread_rwlock_wrlock(&rwlock);
    /* TODO: Write (change register) request shouldn't be cached */
    /* Check for function type */
    // ...

    q = &QGET(rtu->q, 0);
    /* TODO: handle different RTU types */
    slave = q->buf[6];
    func = q->buf[7];
    addr = (q->buf[8] << 8) | q->buf[9];
    nb = (q->buf[10] << 8) | q->buf[11];

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
    memcpy(p->buf, buf, len);
    /* TODO: TTL have to be configured via config for each RTU / slave */
    p->ttd = time(NULL) + CACHE_TTL;
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
    slave = q->buf[6];
    func = q->buf[7];
    addr = (q->buf[8] << 8) | q->buf[9];
    nb = (q->buf[10] << 8) | q->buf[11];

//    DEBUGF("search for sid=%d addr=%d nb=%d\n", slave, addr, nb);

    while (p) {
        DEBUGF("--> (%d,%d,%d) <> (%d,%d,%d)\n",
               func, slave, addr, p->function, p->slaveid, p->addr);
        if (func == p->function && slave == p->slaveid && addr == p->addr)
            return p;
        p = p->next;
    }

    return p;
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
    q.buf = malloc(len);
    q.len = len;
    memcpy(q.buf, buf, len);

    /* Fixup destination slave address */
    q.buf[6] = mi->dst;

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
    rtu->fd = open(rtu->cfg.serial.devname, O_RDWR | O_NONBLOCK);

    return rtu->fd;
}

int rtu_open_tcp(struct rtu_desc *rtu, int port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    char service[NI_MAXSERV];

    sprintf(service, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* Datagram socket */
    hints.ai_flags = AI_NUMERICSERV;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(rtu->cfg.tcp.hostname, service, &hints, &result) != 0) {
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

            if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
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

            if (setnonblocking(s) < 0) {
                close(s);
                continue;
            }

            if (connect(s, rp->ai_addr, rp->ai_addrlen) != 0) {
                if (errno != EINPROGRESS) {
                    close(s);
                    continue;
                }
            }

            rtu->fd = s;
            break;
        }
    }
    freeaddrinfo(result);

    return rtu->fd;
}

int rtu_open_realcom(struct rtu_desc *rtu)
{
    int rc;
 
    rc = rtu_open_tcp(rtu, rtu->cfg.realcom.cmdport);
    if (rc < 0)
        return rc;

    rtu->cfg.realcom.cmdfd = rc;

    rc = rtu_open_tcp(rtu, rtu->cfg.tcp.port);
    if (rc < 0) {
        if (rtu->cfg.realcom.cmdfd != -1)
            close(rtu->cfg.realcom.cmdfd);
        rtu->cfg.realcom.cmdfd = -1;
        rtu->fd = -1;
        return rc;
    }

    realcom_init(rtu);

    return rtu->fd;
}

int rtu_open(struct rtu_desc *rtu, int ep)
{
    int rc = -1;
    struct epoll_event ev;

    switch (rtu->type) {
    case NONE:
        break;
    case RTU:
    case ASCII:
        rc = rtu_open_serial(rtu);
        if (rc < 0) {
            printf("Unable to open %s (%d)\n", rtu->cfg.serial.devname, errno);
        }
        break;
    
    case TCP:
        rc = rtu_open_tcp(rtu, rtu->cfg.tcp.port);
        if (rc < 0) {
            printf("Unable to connect to %s (%d)\n",
                   rtu->cfg.tcp.hostname, errno);
        }
        break;

    case REALCOM:
        rc = rtu_open_realcom(rtu);
        if (rc < 0 || rtu->cfg.realcom.cmdfd < 0) {
            printf("Unable to connect to %s (%d)\n",
                   rtu->cfg.realcom.hostname, errno);
            if (rtu->cfg.realcom.cmdfd != -1)
                close(rtu->cfg.realcom.cmdfd);
            if (rc != -1)
                close(rc);
            rc = -1;
            break;
        }

        ev.data.fd = rtu->cfg.realcom.cmdfd;
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;

        if (epoll_ctl(ep, EPOLL_CTL_ADD, ev.data.fd, &ev) == -1) {
            perror("epoll_ctl(realcom) failed");
            close(rc);
            rtu->cfg.realcom.cmdfd = -1;
            rc = -1;
        }
        break;
    }

    if (rc != -1) {
        ev.data.fd = rc;
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;

        if (epoll_ctl(ep, EPOLL_CTL_ADD, ev.data.fd, &ev) == -1) {
            perror("epoll_ctl(rtu) failed");
            close(rc);
            if (rtu->type == REALCOM) {
                epoll_ctl(ep, EPOLL_CTL_DEL, rtu->cfg.realcom.cmdfd, NULL);
                close(rtu->cfg.realcom.cmdfd);
            }
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
    if (rtu->type == REALCOM) {
        epoll_ctl(ep, EPOLL_CTL_DEL, rtu->cfg.realcom.cmdfd, NULL);
        close(rtu->cfg.realcom.cmdfd);
    }
    rtu->fd = -1;
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
                if (evs[n].events & EPOLLOUT) {
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
                /* Re-open required */
                rtu_close(ri, ep);
                rtu_open(ri, ep);
                fprintf(stderr, "Read failed, trying to re-open %d\n", ri->fd);
                continue;
            }
            DEBUGF("Read %d from %d\n", len, evs[n].data.fd);

            /* Process RealCOM command */
            if (ri->type == REALCOM && ri->fd != evs[n].data.fd) {
                printf("Process CMD %d (%d,%d)\n", len, ri->fd, evs[n].data.fd);
                realcom_process_cmd(ri, buf, len);
                continue;
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
                    _queue_pop(ri);
                    n--;
                    continue;
                } else {
                    /* Check for cache page */
                    p = _cache_find(ri, q);
                    if (p) {
                        DEBUGF("Found, respond to %d len=%d\n",
                               q->resp_fd, p->len);
#if 1
                        /* Revert TID back */
                        p->buf[0] = ri->tido[0];
                        p->buf[1] = ri->tido[1];
#endif
                        write(q->resp_fd, p->buf, p->len);
                        _queue_pop(ri);
                        n--;
                        continue;
                    } else if (q->stamp) {
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
                    } else {
                        write(ri->fd, q->buf+6, q->len-6);
                    }
                    DEBUGF("Write to RTU: %d l=%d\n", ri->fd, q->len);

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
    int ep;
    int cur_child = 0;
    pthread_t rtu_proc;
    pthread_attr_t attr;
    struct sockaddr_in6 sin6;
    struct epoll_event ev;
    struct epoll_event evs[1];
    struct cfg *cfg;
    static struct childs childs[CHILD_NUM+1];

    cfg = cfg_load("mbus.conf");
    if (!cfg) {
        return 1;
    }

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
    if (pthread_create(&rtu_proc, &attr, rtu_thread, cfg) < 0) {
        perror("pthread_create() failed");
        return 2;
    }

    for (n = 0; n < CHILD_NUM; ++n) {
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        childs[n].n = n;
        childs[n].cfg = cfg;
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
