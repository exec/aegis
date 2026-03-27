MUSL_DIR   = ../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none

shell.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f shell.elf
