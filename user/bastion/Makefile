MUSL_DIR   = ../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -I../glyph -I../libauth
TARGET     = bastion.elf
GLYPH      = ../glyph/libglyph.a
AUTH       = ../libauth/libauth.a

$(TARGET): main.c $(GLYPH) $(AUTH)
	$(CC) $(CFLAGS) -o $@ main.c -L../glyph -lglyph -L../libauth -lauth

$(GLYPH):
	$(MAKE) -C ../glyph

$(AUTH):
	$(MAKE) -C ../libauth

clean:
	rm -f $(TARGET) *.o
