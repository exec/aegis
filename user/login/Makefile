CC      = musl-gcc
CFLAGS  = -O2 -static -Wall -Wextra -I../libauth

login.elf: main.c ../libauth/libauth.a
	$(CC) $(CFLAGS) -o login.elf main.c -L../libauth -lauth

../libauth/libauth.a:
	$(MAKE) -C ../libauth

clean:
	rm -f login.elf *.o
