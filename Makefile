GCC  = $(CROSS_COMPILE)gcc -v
CFLAGS = -Wall -Wno-unused-result
LIBS = -lpthread -lyaml -g

C_SRCS = \
	aspp.c \
	cfg.c \
	rtu.c \
	crc16.c \
	

C_OBJS = $(C_SRCS:%.c=%.o)

all: mbus-gw mbus-agent

%.o: %.c
	$(GCC) -O3 -g -c -o $@ $^ $(CFLAGS)

#mbus-gw.o: mbus-gw.h aspp.h vect.h

#aspp.o: aspp.h

mbus-gw: $(C_OBJS) mbus-gw.o
	$(GCC) -o $@ $^ $(LIBS)

mbus-agent: $(C_OBJS) mbus-agent.o
	$(GCC) -o $@ $^ $(LIBS)

clean:
	rm -f $(C_OBJS) mbus-gw.o mbus-agent.o mbuw-gw mbus-agent
