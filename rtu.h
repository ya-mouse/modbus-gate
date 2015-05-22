#ifndef _MBUS_RTU__H
#define _MBUS_RTU__H 1

#define MOXA			0x400
#define MOXA_SET_OP_MODE	(MOXA + 66)
#define MOXA_GET_OP_MODE	(MOXA + 67)

#define RS232_MODE		0
#define RS485_2WIRE_MODE	1
#define RS485_4WIRE_MODE	3

extern int setnonblocking(int sockfd);

extern int rtu_open(struct rtu_desc *rtu, int ep);
extern void rtu_close(struct rtu_desc *rtu, int ep);

#endif /* _MBUS_RTU__H */
