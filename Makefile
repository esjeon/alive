
CC	:= clang
CFLAGS	:= -Wall -lutil

BIN	:= alive

all: $(BIN)

$(BIN): alive.c config.h
	$(CC) $(CFLAGS) -o alive alive.c

config.h: config.def.h
	cp config.def.h config.h

clean:
	@rm -v $(BIN)
