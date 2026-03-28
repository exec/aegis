MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none

all: fb_test.elf

fb_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f *.elf
