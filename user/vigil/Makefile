CC      = musl-gcc
CFLAGS  = -static -O2 -fno-pie -no-pie -Wall -Wextra
TARGET  = vigil

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
