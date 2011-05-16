all: libnanny

libnanny: nanny/libnanny.so
	cd nanny; make

