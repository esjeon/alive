
CC	?= clang
CFLAGS	+= -g -Wall -std=c99 -pedantic
CFLAGS  += -D_POSIX_C_SOURCE=200112L -D_BSD_SOURCE
CFLAGS  += -Os -lutil

all: alive

alive: alive.c config.h
	$(CC) $(CFLAGS) -o alive alive.c

config.h: config.def.h
	cp config.def.h config.h

clean:
	@rm -fv alive
