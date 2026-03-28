MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall

SRCS = main.c draw.c cursor.c compositor.c taskbar.c terminal.c
OBJS = $(SRCS:.c=.o)

all: lumen.elf

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lumen.elf: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f *.elf *.o
