CC     = musl-gcc
CFLAGS = -static -O2 -fno-pie -no-pie -Wl,--build-id=none

hello.elf: main.c
	$(CC) $(CFLAGS) -o hello.elf main.c

clean:
	rm -f hello.elf
