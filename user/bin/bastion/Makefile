MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -I../../lib/glyph -I../../lib/libauth
TARGET     = bastion.elf
GLYPH      = ../../lib/glyph/libglyph.a
AUTH       = ../../lib/libauth/libauth.a

$(TARGET): main.c $(GLYPH) $(AUTH)
	$(CC) $(CFLAGS) -o $@ main.c -L../../lib/glyph -lglyph -L../../lib/libauth -lauth

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

$(AUTH):
	$(MAKE) -C ../../lib/libauth

clean:
	rm -f $(TARGET) *.o
