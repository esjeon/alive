
CC	:= clang
CFLAGS	:= -Wall -lutil

BIN	:= alive

all: $(BIN)

$(BIN): alive.c
	$(CC) $(CFLAGS) -o alive alive.c

clean:
	@rm -v $(BIN)
