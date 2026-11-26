# cleanroom/Makefile - Portable POSIX fallback when CMake is not installed
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -pedantic -O2
INCLUDES := -Iinclude

CORE_SRCS := src/interlnk.c src/crc16.c src/protocol.c src/storage_mem.c
CORE_OBJS := $(CORE_SRCS:.c=.o)

.PHONY: all test clean

all: libinterlnk.a ilnk-server ilnk-client ilnk-vector-check test_libinterlnk

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

libinterlnk.a: $(CORE_OBJS)
	$(AR) rcs $@ $(CORE_OBJS)

ilnk-server: desktop/main_server.c libinterlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) desktop/main_server.c libinterlnk.a -o $@

ilnk-client: desktop/main_client.c libinterlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) desktop/main_client.c libinterlnk.a -o $@

ilnk-vector-check: desktop/vector_check.c libinterlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) desktop/vector_check.c libinterlnk.a -o $@

test_libinterlnk: tests/test_libinterlnk.c libinterlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_libinterlnk.c libinterlnk.a -o $@

test: all
	./test_libinterlnk
	./ilnk-server --self-test

clean:
	rm -f $(CORE_OBJS) libinterlnk.a ilnk-server ilnk-client ilnk-vector-check test_libinterlnk
