MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -I../glyph -I../citadel

SRCS = main.c cursor.c compositor.c terminal.c
OBJS = $(SRCS:.c=.o)
GLYPH = ../glyph/libglyph.a
CITADEL = ../citadel/libcitadel.a

all: lumen.elf

$(GLYPH):
	$(MAKE) -C ../glyph

$(CITADEL):
	$(MAKE) -C ../citadel

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lumen.elf: $(OBJS) $(GLYPH) $(CITADEL)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -L../citadel -lcitadel -L../glyph -lglyph

clean:
	rm -f *.elf *.o
