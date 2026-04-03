MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wall
AR = ar

SRCS = draw.c widget.c window.c box.c label.c button.c textfield.c \
       checkbox.c progress.c image.c scrollbar.c listview.c menubar.c tabs.c \
       font.c
OBJS = $(SRCS:.c=.o)

all: libglyph.a

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

libglyph.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f libglyph.a *.o
