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

#define CHILD_NUM       4
#define BUF_SIZE        512
#define MAX_EVENTS      1024
#define MAX_RTU_EVENTS  128
#define MODBUS_TCP_PORT 502
#define RTU_TIMEOUT     3

struct cache_page {
    int status;         /* 0 - ok, 1 - timeout, 2 - NA */
    int slaveid;
    int addr;
    int len;
    time_t ttd;         /* time to die of the page: last_timestamp + TTL */
    struct cache_page *next;
};

struct queue_list {
    int resp_fd;    /* "response to" descriptor */
    u_int8_t *buf;  /* request buffer */
    size_t len;     /* request length */
    time_t stamp;   /* timestamp of timeout: last_timestamp + timeout */
    struct queue_list *next;
};

struct rtu_desc {
    int fd;               /* ttySx descriptior */
    int slave_id;         /* slave_id for MODBUS-TCP */
    struct queue_list *q; /* queue list */
};

struct childs {
    int n;
    int ep;
    pthread_t th;
};

static struct rtu_desc rtu[MAX_RTU_EVENTS];
static struct cache_page cache;
static pthread_rwlock_t rwlock;

struct queue_list *queue_from_slaveid(int slave_id)
{
    int i;
    struct queue_list *q = NULL;

    pthread_rwlock_rdlock(&rwlock);
    for (i = 0; i < MAX_RTU_EVENTS; ++i) {
        if (!rtu[i].fd)
            break;
        if (rtu[i].slave_id == slave_id) {
            q = rtu[i].q;
            break;
        }
    }
    pthread_rwlock_unlock(&rwlock);

    return q;
}

int queue_add(struct queue_list *que, int fd, u_int8_t *buf, size_t len)
{
    struct queue_list *q = que;

    if (!que)
        return -1;

    pthread_rwlock_wrlock(&rwlock);
    if (!q) {
        que = q = malloc(sizeof(struct queue_list));
    } else {
        while (q->next != NULL)
            q = q->next;
        q->next = malloc(sizeof(struct queue_list));
        q = q->next;
    }
    q->resp_fd = fd;
    q->buf = buf;
    q->len = len;
    pthread_rwlock_unlock(&rwlock);

    return 0;
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
    struct queue_list *q;
    u_int8_t buf[BUF_SIZE];
    struct epoll_event ev;
    struct epoll_event evs[MAX_RTU_EVENTS];

    ep = epoll_create(MAX_RTU_EVENTS);
    if (ep == -1) {
        perror("epoll_create() failed");
        return NULL;
    }

    /* Load RTU config */
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = 0;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ev.data.fd, &ev) == -1) {
        perror("epoll_ctl(0) failed");
        return NULL;
    }

    memset(&cache, 0, sizeof(struct cache_page));

    for (;;) {
        int n;
        int is_top = 1;
        int nfds = epoll_wait(ep, evs, MAX_RTU_EVENTS, 500);
        if (nfds == -1) {
            if (nfds == -EINTR)
                continue;
            perror("epoll_wait() failed");
            goto err;
        }

        /* Process RTU data */
        for (n = 0; n < nfds; ++n) {
            int len;
            if (!(evs[n].events & EPOLLIN))
                continue;

            if (len <= 0) {
                /* Re-open requested */
            }

            len = read(evs[n].data.fd, buf, BUF_SIZE);

            /* Update cache */
        }

        /* Process QUEUE */
        pthread_rwlock_wrlock(&rwlock);
        for (n = 0; n < MAX_RTU_EVENTS; ++n) {
            if (!rtu[n].fd)
                break;

            q = rtu[n].q;
            while (q) {
                /* Check for timeouted items */
                // ...

                /* Process new request */
                if (!q->stamp) {
                    /* Find slave_id in the mappings */
                    // ...
                    if (is_top) {
                        /* Write req */
                    }
                    q->stamp = time(NULL) + RTU_TIMEOUT;
                }
    //            is_top = 0;

                q = q->next;
            }
        }
        pthread_rwlock_unlock(&rwlock);

        /* Invalidate cache pages */
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
            if (nfds == -EINTR)
                continue;
            perror("epoll_wait() failed");
            goto err;
        }

        for (n = 0; n < nfds; ++n) {
            int len;
            if (!(evs[n].events & EPOLLIN))
                continue;

            len = recv(evs[n].data.fd, buf, BUF_SIZE, 0);
            if (len == 0) {
c_close:
                epoll_ctl(self->ep, EPOLL_CTL_DEL, evs[n].data.fd, NULL);
                close(evs[n].data.fd);
            } else if (len < 0) {
                perror("Error occured");
            } else {
                char ans[] = "HTTP/1.1 200 OK\r\nServer: fake/1.0\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html>OK</html>\r\n";
                int sz = sizeof(ans);
                send(evs[n].data.fd, ans, sz, 0);

//                queue_add(queue_from_slaveid(buf[6]), evs[n].data.fd, buf, len);

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
    pthread_t rtu;
    pthread_attr_t attr;
    struct sockaddr_in6 sin6;
    struct epoll_event ev;
    struct epoll_event evs[1];
    static struct childs childs[CHILD_NUM];

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
    if (pthread_create(&rtu, &attr, rtu_thread, NULL) < 0) {
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
            if (nfds == -EINTR)
                continue;            
            perror("epoll_wait() failed");
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
