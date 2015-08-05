#ifndef _ASPP__H
#define _ASPP__H 1

/*
 *      from original npreal2.c  -- MOXA NPort Server family Real TTY driver.
 *
 *      Copyright (C) 1999-2007  Moxa Technologies (support@moxa.com.tw).
 *                         2014  Anton D. Kachalov (mouse@yandex.ru).
 *
 *      This code is loosely based on the Linux serial driver, written by
 *      Linus Torvalds, Theodore T'so and others.
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 */

#define	NPREAL_PORTS	 256

#define	DE211	211
#define	DE311	311
#define	DE301	301
#define	DE302	302
#define	DE304	304
#define	DE331	331
#define	DE332	332
#define	DE334	334
#define	DE303	303
#define	DE308	308
#define	DE309	309
#define	CN2100	2100
#define	CN2500	2500

#ifndef B921600
#define	B921600	(B460800 + 1)
#endif

#define	NPREAL_ASPP_COMMAND_SET		1
#define	NPREAL_LOCAL_COMMAND_SET	2

// local command set
#define	LOCAL_CMD_TTY_USED			1
#define	LOCAL_CMD_TTY_UNUSED		2
#define NPREAL_NET_CONNECTED		3
#define NPREAL_NET_DISCONNECTED		4
#define NPREAL_NET_SETTING			5
#define NPREAL_NET_GET_TTY_STATUS	6

#define	NPREAL_CMD_TIMEOUT		10*HZ  // 10 seconds

#define	NPREAL_NET_CMD_RETRIEVE		1
#define	NPREAL_NET_CMD_RESPONSE		2


#define	NPREAL_NET_NODE_OPENED			0x01
#define	NPREAL_NET_NODE_CONNECTED		0x02
#define	NPREAL_NET_NODE_DISCONNECTED	0x04
#define	NPREAL_NET_DO_SESSION_RECOVERY	0x08
#define	NPREAL_NET_DO_INITIALIZE		0x10
#define	NPREAL_NET_TTY_INUSED			0x20

// ASPP command set

#define	ASPP_CMD_NOTIFY 		0x26
#define	ASPP_CMD_POLLING	    0x27
#define	ASPP_CMD_ALIVE		    0x28

#define	ASPP_NOTIFY_PARITY 		0x01
#define	ASPP_NOTIFY_FRAMING 	0x02
#define	ASPP_NOTIFY_HW_OVERRUN 	0x04
#define	ASPP_NOTIFY_SW_OVERRUN 	0x08
#define	ASPP_NOTIFY_BREAK 		0x10
#define	ASPP_NOTIFY_MSR_CHG 	0x20

#define	ASPP_CMD_IOCTL			16
#define	ASPP_CMD_FLOWCTRL		17
#define	ASPP_CMD_LSTATUS		19
#define	ASPP_CMD_LINECTRL		18
#define	ASPP_CMD_FLUSH			20
#define	ASPP_CMD_OQUEUE			22
#define	ASPP_CMD_SETBAUD		23
#define	ASPP_CMD_START_BREAK	33
#define	ASPP_CMD_STOP_BREAK		34
#define	ASPP_CMD_START_NOTIFY	36
#define	ASPP_CMD_STOP_NOTIFY	37
#define	ASPP_CMD_HOST			43
#define	ASPP_CMD_PORT_INIT		44
#define	ASPP_CMD_WAIT_OQUEUE 	47

#define	ASPP_CMD_IQUEUE			21
#define	ASPP_CMD_XONXOFF		24
#define	ASPP_CMD_PORT_RESET		32
#define	ASPP_CMD_RESENT_TIME	46
#define	ASPP_CMD_TX_FIFO		48
#define ASPP_CMD_SETXON     	51
#define ASPP_CMD_SETXOFF    	52

#define	ASPP_FLUSH_RX_BUFFER	0
#define	ASPP_FLUSH_TX_BUFFER	1
#define	ASPP_FLUSH_ALL_BUFFER	2

#define	ASPP_IOCTL_B300			0
#define	ASPP_IOCTL_B600			1
#define	ASPP_IOCTL_B1200		2
#define	ASPP_IOCTL_B2400		3
#define	ASPP_IOCTL_B4800		4
#define	ASPP_IOCTL_B7200		5
#define	ASPP_IOCTL_B9600		6
#define	ASPP_IOCTL_B19200		7
#define	ASPP_IOCTL_B38400		8
#define	ASPP_IOCTL_B57600		9
#define	ASPP_IOCTL_B115200		10
#define	ASPP_IOCTL_B230400		11
#define	ASPP_IOCTL_B460800		12
#define	ASPP_IOCTL_B921600		13
#define	ASPP_IOCTL_B150			14
#define	ASPP_IOCTL_B134			15
#define	ASPP_IOCTL_B110			16
#define	ASPP_IOCTL_B75			17
#define	ASPP_IOCTL_B50			18

#define	ASPP_IOCTL_BITS8		3
#define	ASPP_IOCTL_BITS7		2
#define	ASPP_IOCTL_BITS6		1
#define	ASPP_IOCTL_BITS5		0

#define	ASPP_IOCTL_STOP1		0
#define	ASPP_IOCTL_STOP2		4

#define	ASPP_IOCTL_EVEN			8
#define	ASPP_IOCTL_ODD			16
#define	ASPP_IOCTL_MARK			24
#define	ASPP_IOCTL_SPACE		32
#define	ASPP_IOCTL_NONE			0

extern int realcom_init(struct rtu_desc *rtu);
extern int realcom_process_cmd(struct rtu_desc *rtu,
                               const uint8_t *buf,
                               size_t len);

#endif /* _ASPP__H */
