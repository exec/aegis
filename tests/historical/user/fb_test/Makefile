MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -I../glyph

all: fb_test.elf

fb_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
