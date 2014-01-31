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

#mbus-gw.o: mbus-gw.h aspp.h vect.h

#aspp.o: aspp.h

mbus-gw: $(C_OBJS)
	$(GCC) -o $@ $^ $(LIBS)
