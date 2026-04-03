MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none

all: mouse_test.elf

mouse_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
