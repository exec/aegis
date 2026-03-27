CC     = musl-gcc
CFLAGS = -static -O2 -fno-pie -no-pie -Wl,--build-id=none -lpthread

thread_test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f thread_test.elf
