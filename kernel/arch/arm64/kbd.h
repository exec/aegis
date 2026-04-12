#ifndef AEGIS_KBD_H
#define AEGIS_KBD_H

#include <stdint.h>

/* ARM64 keyboard stubs — no PS/2 keyboard on QEMU virt.
 * Input will come from virtio-input or PL011 console in a later phase. */

void     kbd_init(void);
void     kbd_handler(void);
char     kbd_read(void);
int      kbd_poll(char *out);
int      kbd_has_data(void);
char     kbd_read_interruptible(int *interrupted);
void     kbd_set_tty_pgrp(uint32_t pgid);
uint32_t kbd_get_tty_pgrp(void);

#endif /* AEGIS_KBD_H */
