CC = aarch64-elf-gcc
LD = aarch64-elf-ld

all: init.elf

init.elf: init.S link.ld
	$(CC) -nostdlib -static -T link.ld -o $@ $<

link.ld:
	@echo 'SECTIONS { . = 0x41000000; .text : { *(.text) } }' > $@

clean:
	rm -f init.elf
