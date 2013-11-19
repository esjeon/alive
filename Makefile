
CC	?= clang
CFLAGS	+= -g -Wall -std=c99 -pedantic
CFLAGS  += -Os -lutil

all: alive

alive: alive.c config.h
	$(CC) $(CFLAGS) -o alive alive.c

config.h: config.def.h
	cp config.def.h config.h

clean:
	@rm -fv alive
