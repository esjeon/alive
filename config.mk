
# flags
CPPFLAGS = -D_POSIX_C_SOURCE=200112L -D_BSD_SOURCE -D __BSD_VISIBLE=1
CFLAGS += -g -std=c99 -pedantic -Wall -Os ${CPPFLAGS} 
LDFLAGS += -lutil

# compiler and linker
CC ?= cc

