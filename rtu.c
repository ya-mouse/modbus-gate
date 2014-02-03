#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netdb.h>

#include "cfg.h"
#include "rtu.h"
#include "common.h"
#include "aspp.h"

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

int rtu_open_unix(struct rtu_desc *rtu)
{
    struct sockaddr_un name;

    if ((rtu->fd = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
        perror("socket(PF_LOCAL) failed");
        rtu->fd = -1;
        goto out;
    }

    memset(&name, 0, sizeof(name));

    name.sun_family = AF_LOCAL;
    strcpy(name.sun_path, rtu->cfg.name.sockfile);

    if (connect(rtu->fd, (struct sockaddr *)&name, SUN_LEN(&name)) != 0) {
        if (errno != EINPROGRESS) {
            close(rtu->fd);
            rtu->fd = -1;
            goto out;
        }
    }

out:
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

    rtu->retries++;
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

    if (rtu->retries > CFG_MAX_RETRIES) {
        return -1;
    }

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

    case UNIX:
        rc = rtu_open_unix(rtu);
        if (rc < 0) {
            printf("Unable to open %s (%d)\n",
                   rtu->cfg.name.sockfile, errno);
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
    if (rtu->fd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, rtu->fd, NULL);
        close(rtu->fd);
    }
    if (rtu->type == REALCOM && rtu->cfg.realcom.cmdfd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, rtu->cfg.realcom.cmdfd, NULL);
        close(rtu->cfg.realcom.cmdfd);
        rtu->cfg.realcom.cmdfd = -1;
    }
    rtu->fd = -1;
}
