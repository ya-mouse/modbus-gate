#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/epoll.h>
#ifndef _NUTTX_BUILD
#include <netinet/tcp.h>
#endif
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netdb.h>
#include "cfg.h"
#include "rtu.h"
#include "common.h"
#ifndef _NUTTX_BUILD
#include "aspp.h"
#endif

#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

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
    int v = -1;
    struct termios options;

    /* TODO: setup baud rate, bits and parity */
    printf("Opening (%s)\n", rtu->cfg.serial.devname);
    rtu->fd = open(rtu->cfg.serial.devname, O_RDWR | O_NONBLOCK);
    if (rtu->fd != -1) {
        tcgetattr(rtu->fd, &options);
        options.c_iflag = 0;
        options.c_oflag &= ~OPOST;
        options.c_lflag &= ~(ISIG | ICANON
#ifdef XCASE
                                  | XCASE
#endif
                            );
        options.c_cflag &= ~(CSIZE | PARENB);
        if (rtu->cfg.serial.t.c_cflag == 0)
            rtu->cfg.serial.t.c_cflag = CS8 | rtu->conf->baud;
        options.c_cflag = rtu->cfg.serial.t.c_cflag;
        options.c_cc[VMIN] = 1;
        options.c_cc[VTIME] = 0;
        tcsetattr(rtu->fd, TCSANOW, &options);
        ioctl(rtu->fd, MOXA_GET_OP_MODE, &v);
        printf("opmode=%d\n", v);
        v = RS485_2WIRE_MODE;
        ioctl(rtu->fd, MOXA_SET_OP_MODE, &v);
    }
    printf("-> fd=%d\n", rtu->fd);

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

#ifndef _NUTTX_BUILD
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
#endif

    rtu->retries++;
    return rtu->fd;
}

#ifndef _NUTTX_BUILD
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
#endif

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

#ifndef _NUTTX_BUILD
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
#endif
    }

    if (rc != -1) {
        ev.data.fd = rc;
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;

        if (epoll_ctl(ep, EPOLL_CTL_ADD, ev.data.fd, &ev) == -1) {
            perror("epoll_ctl(rtu) failed");
            close(rc);
#ifndef _NUTTX_BUILD
            if (rtu->type == REALCOM) {
                epoll_ctl(ep, EPOLL_CTL_DEL, rtu->cfg.realcom.cmdfd, NULL);
                close(rtu->cfg.realcom.cmdfd);
            }
#endif
            rc = -1;
        }
    }

    rtu->fd = rc;
    return rc;
}

void rtu_close(struct rtu_desc *rtu, int ep)
{
    if (rtu->fd > 0) {
        epoll_ctl(ep, EPOLL_CTL_DEL, rtu->fd, NULL);
        close(rtu->fd);
    }
#ifndef _NUTTX_BUILD
    if (rtu->type == REALCOM && rtu->cfg.realcom.cmdfd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, rtu->cfg.realcom.cmdfd, NULL);
        close(rtu->cfg.realcom.cmdfd);
        rtu->cfg.realcom.cmdfd = -1;
    }
#endif
    rtu->fd = -1;
}
