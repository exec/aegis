CC      = musl-gcc
CFLAGS  = -O2 -static -Wall -Wextra

login.elf: main.c
	$(CC) $(CFLAGS) -o login.elf main.c

clean:
	rm -f login.elf *.o
