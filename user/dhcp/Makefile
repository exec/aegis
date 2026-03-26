CC      = musl-gcc
CFLAGS  = -O2 -static -Wall -Wextra
TARGET  = dhcp

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET)
