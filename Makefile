GCC  = gcc
LIBS = -lpthread -g

C_SRCS = mbus-gw.c

C_OBJS = $(C_SRCS:%.c=%.o)

all: mbus-gw

%.o: %.c
	$(GCC) -O3 -g -c -o $@ $^ -Wall

mbus-gw: $(C_OBJS)
	$(GCC) -o $@ $^ $(LIBS)
