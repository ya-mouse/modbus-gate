GCC  = gcc
CFLAGS = -Wall -Wno-unused-result
LIBS = -lpthread -g

C_SRCS = \
	mbus-gw.c \
	aspp.c \
	

C_OBJS = $(C_SRCS:%.c=%.o)

all: mbus-gw

%.o: %.c
	$(GCC) -O3 -g -c -o $@ $^ $(CFLAGS)

mbus-gw: $(C_OBJS)
	$(GCC) -o $@ $^ $(LIBS)
