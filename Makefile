
include config.mk

SRC = alive.c
OBJ = ${SRC:.c=.o}

all: options alive

options:
	@echo alive build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

alive: ${OBJ}
	@echo CC -o $@
	@${CC} ${LDFLAGS} -o $@ ${OBJ}

config.h: config.def.h
	cp config.def.h config.h

clean:
	@echo cleaning
	@rm -fv alive ${OBJ}
