GCC  = gcc
CFLAGS = -Wall -Wno-unused-result
LIBS = -lpthread -g

C_SRCS = \
	mbus-gw.c \
	aspp.c \
	

C_OBJS = $(C_SRCS:%.c=%.o)

all: mbus-gw

mbus-gw.c: mbus-gw.h aspp.h vect.h

aspp.c: aspp.h

.o.c:
	$(GCC) -O3 -g -c -o $@ $^ $(CFLAGS)

mbus-gw: $(C_OBJS)
	$(GCC) -o $@ $^ $(LIBS)
