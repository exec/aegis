MUSL_DIR   = ../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wl,--build-id=none
TARGET     = proc_test.elf

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
