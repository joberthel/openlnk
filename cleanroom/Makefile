# cleanroom/Makefile - Portable POSIX fallback when CMake is not installed
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -pedantic -O2
INCLUDES := -Iinclude

CORE_SRCS := src/openlnk.c src/crc16.c src/protocol.c src/storage_mem.c src/server.c src/client.c
CORE_OBJS := $(CORE_SRCS:.c=.o)

.PHONY: all test clean

all: libopenlnk.a opensvr openlnk openlnk-vector-check test_libopenlnk test_dos_hal

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

libopenlnk.a: $(CORE_OBJS)
	$(AR) rcs $@ $(CORE_OBJS)

opensvr: desktop/main_server.c desktop/vfat_dir.c include/openlnk/version.h libopenlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) desktop/main_server.c desktop/vfat_dir.c libopenlnk.a -o $@

openlnk: desktop/main_client.c include/openlnk/version.h libopenlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) desktop/main_client.c libopenlnk.a -o $@

openlnk-vector-check: desktop/vector_check.c libopenlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) desktop/vector_check.c libopenlnk.a -o $@

test_libopenlnk: tests/test_libopenlnk.c libopenlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_libopenlnk.c libopenlnk.a -o $@

test_dos_hal: tests/test_dos_hal.c dos/dos_hal_uart.c dos/dos_hal_disk.c dos/dos_hal_io.c dos/openlnk_sys_stub.c libopenlnk.a
	$(CC) $(CFLAGS) $(INCLUDES) -Idos tests/test_dos_hal.c dos/dos_hal_uart.c dos/dos_hal_disk.c dos/dos_hal_io.c dos/openlnk_sys_stub.c libopenlnk.a -o $@

test: all
	./test_libopenlnk
	./test_dos_hal
	./opensvr --self-test
	./openlnk --self-test

clean:
	rm -f $(CORE_OBJS) libopenlnk.a opensvr openlnk openlnk-vector-check test_libopenlnk test_dos_hal
