MUSL_GCC ?= ../../build/musl-dynamic/usr/bin/musl-gcc
CC       = $(MUSL_GCC)
CFLAGS   = -O2 -static -Wall -Wextra -Wno-unused-parameter

pwn.elf: pwn.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f pwn.elf *.o
