CFLAGS= -g -Wall -O2 -fPIC
LDFLAGS= -g -Wall

OBJS =	nanny_children.o	\
	nanny_core.o		\
	nanny_counter.o		\
	nanny_http_server.o	\
	nanny_log.o		\
	nanny_timer.o		\
	nanny_udp_server.o	\
	nanny_utility.o		\
	nanny_variable.o	\
	strlcpy.o		\
	strlcat.o

all: nanny
	-cd test && make

.PHONY: all clean check

check:
	-cd test && make check

clean:
	-rm -f *.o *~
	-rm -rf *.dSYM
	-rm -f nanny wont
	-cd test && make clean

nanny: nanny_main.o ${OBJS}
	gcc ${LDFLAGS} -o nanny ${OBJS}

nanny_so: ${OBJS}
	gcc -fPIC ${LDFLAGS} -shared -o libnanny.so ${OBJS}

nanny_core.o: nanny_core.c nanny.h

nanny_udp_server.o: nanny_udp_server.c nanny.h

nanny_http_server.o: nanny_http_server.c nanny.h

nanny_log.o: nanny_log.c nanny.h

nanny_timer.o: nanny_timer.c nanny_timer.h

nanny_main.o: nanny_main.c nanny.h

nanny_variable.o: nanny_variable.c nanny.h

nanny_children.o: nanny_children.c nanny.h

strlcpy.o: strlcpy.c

strlcat.o: strlcat.c
