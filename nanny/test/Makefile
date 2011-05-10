CFLAGS= -g -Wall -pedantic -O2 -I..
LDFLAGS= -g -Wall -pedantic

all: wont

.PHONY: all clean

wont: wont.c
	gcc ${CFLAGS} -o wont wont.c

check: timer_test
	./timer_test

timer_test: timer_test.c ../nanny_timer.c

clean:
	-rm -f *.o *~
	-rm -rf *.dSYM
	-rm -f wont
