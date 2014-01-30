#include <unistd.h>
#include <linux/serial_reg.h>

#include "mbus-gw.h"
#include "aspp.h"

int realcom_init(struct rtu_desc *rtu)
{
    u_int8_t cmd[10];
    char baud;
    int mode;

    mode = rtu->cfg.realcom.t.c_cflag & CSIZE;
    if (mode == CS5)
        mode = ASPP_IOCTL_BITS5;
    else if (mode == CS6)
        mode = ASPP_IOCTL_BITS6;
    else if (mode == CS7)
        mode = ASPP_IOCTL_BITS7;
    else if (mode == CS8)
        mode = ASPP_IOCTL_BITS8;

    if (rtu->cfg.realcom.t.c_cflag & CSTOPB)
        mode |= ASPP_IOCTL_STOP2;
    else
        mode |= ASPP_IOCTL_STOP1;

    if (rtu->cfg.realcom.t.c_cflag & PARENB)
    {
#ifdef CMSPAR
        if (rtu->cfg.realcom.t.c_cflag & CMSPAR)
            if (rtu->cfg.realcom.t.c_cflag & PARODD)
                mode |= ASPP_IOCTL_MARK;
            else
                mode |= ASPP_IOCTL_SPACE;
        else
#endif
            if (rtu->cfg.realcom.t.c_cflag & PARODD)
                mode |= ASPP_IOCTL_ODD;
            else
                mode |= ASPP_IOCTL_EVEN;
    }
    else
        mode |= ASPP_IOCTL_NONE;

    switch (rtu->cfg.realcom.t.c_cflag & (CBAUD|CBAUDEX))
    {
    case B921600:
        baud = ASPP_IOCTL_B921600;
        break;
    case B460800:
        baud = ASPP_IOCTL_B460800;
        break;
    case B230400:
        baud = ASPP_IOCTL_B230400;
        break;
    case B115200:
        baud = ASPP_IOCTL_B115200;
        break;
    case B57600:
        baud = ASPP_IOCTL_B57600;
        break;
    case B38400:
        baud = ASPP_IOCTL_B38400;
        if ( (rtu->cfg.realcom.flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI )
        {
            baud = ASPP_IOCTL_B57600;
        }
        if ( (rtu->cfg.realcom.flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI )
        {
            baud = ASPP_IOCTL_B115200;
        }

#ifdef ASYNC_SPD_SHI
        if ((rtu->cfg.realcom.flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
        {
            baud = ASPP_IOCTL_B230400;
        }
#endif

#ifdef ASYNC_SPD_WARP
        if ((rtu->cfg.realcom.flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
        {
            baud = ASPP_IOCTL_B460800;
        }
#endif
        break;
    case B19200:
        baud = ASPP_IOCTL_B19200;
        break;
    case B9600:
        baud = ASPP_IOCTL_B9600;
        break;
    case B4800:
        baud = ASPP_IOCTL_B4800;
        break;
    case B2400:
        baud = ASPP_IOCTL_B2400;
        break;
    case B1800:
        baud = 0xff;
        break;
    case B1200:
        baud = ASPP_IOCTL_B1200;
        break;
    case B600:
        baud = ASPP_IOCTL_B600;
        break;
    case B300:
        baud = ASPP_IOCTL_B300;
        break;
    case B200:
        baud = 0xff;
        break;
    case B150:
        baud = ASPP_IOCTL_B150;
        break;
    case B134:
        baud = ASPP_IOCTL_B134;
        break;
    case B110:
        baud = ASPP_IOCTL_B110;
        break;
    case B75:
        baud = ASPP_IOCTL_B75;
        break;
    case B50:
        baud = ASPP_IOCTL_B50;
        break;
    default:
        baud = 0xff;
    }

    cmd[0] = ASPP_CMD_PORT_INIT;

    cmd[1] = 8;
//
// baud rate
//
    cmd[2] = baud;
//
// mode
//
    cmd[3] = mode;
//
// line control
//
    if (rtu->cfg.realcom.modem_control & UART_MCR_DTR)
        cmd[4] = 1;
    else
        cmd[4] = 0;
    if (rtu->cfg.realcom.modem_control & UART_MCR_RTS)
        cmd[5] = 1;
    else
        cmd[5] = 0;
//
// flow control
//
    if (rtu->cfg.realcom.t.c_cflag & CRTSCTS)
    {
        cmd[6] = 1;
        cmd[7] = 1;
    }
    else
    {
        cmd[6] = 0;
        cmd[7] = 0;
    }
    if (rtu->cfg.realcom.t.c_iflag & IXON)
    {
        cmd[8] = 1;
    }
    else
    {
        cmd[8] = 0;
    }
    if (rtu->cfg.realcom.t.c_iflag & IXOFF)
    {
        cmd[9] = 1;
    }
    else
    {
        cmd[9] = 0;
    }

    return write(rtu->cfg.realcom.cmdfd, cmd, sizeof(cmd));
}

int realcom_process_cmd(struct rtu_desc *rtu, const u_int8_t *buf, size_t len)
{
    int nr;
    int i = 0;
    u_int8_t cmd[3];

    while (len) {
        switch (buf[i]) {
        case ASPP_CMD_POLLING:
            if (len < 3) {
                len = 0;
                continue;
            }
            cmd[0] = ASPP_CMD_ALIVE;
            cmd[1] = 1;
            cmd[2] = buf[i+2];
            nr = write(rtu->cfg.realcom.cmdfd, cmd, sizeof(cmd));
            break;
 
        case ASPP_CMD_NOTIFY:
        case ASPP_CMD_WAIT_OQUEUE:
        case ASPP_CMD_OQUEUE:
        case ASPP_CMD_IQUEUE:
            nr = 4;
            break;
        case ASPP_CMD_LSTATUS:
        case ASPP_CMD_PORT_INIT:
            nr = 5;
            break;
        case ASPP_CMD_FLOWCTRL:
        case ASPP_CMD_IOCTL:
        case ASPP_CMD_SETBAUD:
        case ASPP_CMD_LINECTRL:
        case ASPP_CMD_START_BREAK:
        case ASPP_CMD_STOP_BREAK:
        case ASPP_CMD_START_NOTIFY:
        case ASPP_CMD_STOP_NOTIFY:
        case ASPP_CMD_FLUSH:
        case ASPP_CMD_HOST:
        case ASPP_CMD_TX_FIFO:
        case ASPP_CMD_XONXOFF:
        case ASPP_CMD_SETXON:
        case ASPP_CMD_SETXOFF:
            nr = 3;
            break;
        default:
            nr = len;
            break;
        }
        i += nr;
        len -= nr;
    }

    return len;
}
