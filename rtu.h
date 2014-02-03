#ifndef _MBUS_RTU__H
#define _MBUS_RTU__H 1

extern int setnonblocking(int sockfd);

extern int rtu_open(struct rtu_desc *rtu, int ep);
extern void rtu_close(struct rtu_desc *rtu, int ep);

#endif /* _MBUS_RTU__H */
