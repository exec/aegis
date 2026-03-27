MUSL_DIR   = ../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wl,--build-id=none -lpthread

thread_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f thread_test.elf
