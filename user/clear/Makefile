CC     = musl-gcc
CFLAGS = -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none

clear.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
