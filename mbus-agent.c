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

#include "mbus-agent.h"
#include "aspp.h"
#include "cfg.h"
#include "rtu.h"

static struct cfg *config = NULL;

void *rtu_thread(void *arg)
{
    int ep;
    struct rtu_desc *ri;
    uint8_t buf[BUF_SIZE];
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
                if (evs[n].events & EPOLLHUP) {
                    goto reconnect;
                } else if (evs[n].events & EPOLLOUT) {
//                    epoll_ctl(self->ep, EPOLL_CTL_DEL, evs[n].data.fd, NULL);
//                    printf("ev=%x\n", evs[n].events);
                }
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
            DEBUGF("Read %d from %d\n", len, evs[n].data.fd);

            /* Process RealCOM command */
            if (ri->type == REALCOM && ri->fd != evs[n].data.fd) {
                printf("Process CMD %d (%d,%d)\n", len, ri->fd, evs[n].data.fd);
                realcom_process_cmd(ri, buf, len);
                continue;
            }
        }

        cur_time = time(NULL);
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
    int ep;
    int cur_child = 0;
    pthread_t rtu_proc;
    pthread_attr_t attr;
    struct rtu_desc *ri;
    struct epoll_event ev;
    struct epoll_event *evs;
    static struct workers *workers;

    config = cfg_load("mbus.conf");
    if (!config) {
        return 1;
    }

    ep = epoll_create(VLEN(config->rtu_list));
    if (ep == -1) {
        perror("epoll_create() failed");
        return 1;
    }

    evs = malloc(sizeof(struct epoll_event) * VLEN(config->rtu_list));

    VFOREACH(config->rtu_list, ri) {
        rtu_open(ri, ep);
    }

#if 0
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&rtu_proc, &attr, rtu_thread, cfg) < 0) {
        perror("pthread_create() failed");
        return 2;
    }

    childs = malloc(sizeof(struct childs) * cfg->workers);

    for (n = 0; n < cfg->workers; ++n) {
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
                if (epoll_ctl(childs[cur_child++].ep, EPOLL_CTL_ADD, c, &ev) < 0) {
                    perror("epoll_ctl ADD()");
                    close(c);
                }
                cur_child %= cfg->workers;
            }
        }
    }

#endif

    return 0;
}
