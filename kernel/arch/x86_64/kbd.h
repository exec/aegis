#ifndef AEGIS_KBD_H
#define AEGIS_KBD_H

#include <stdint.h>

/* Initialize PS/2 keyboard. Unmasks IRQ1.
 * Prints [KBD] OK: PS/2 keyboard ready. */
void kbd_init(void);

/* Called by isr_dispatch on vector 0x21.
 * Reads scancode from port 0x60, converts to ASCII, pushes to ring buffer.
 * Break codes (bit 7 set) and 0xE0 extended scancodes are silently dropped. */
void kbd_handler(void);

/* Blocking read — spins until a character is available in the ring buffer. */
char kbd_read(void);

/* Non-blocking read. Returns 1 and writes to *out if a char is available.
 * Returns 0 if the buffer is empty. */
int kbd_poll(char *out);

/* Register the foreground process group for signal delivery.
 * Called by the shell via sys_setfg (syscall 360) before waitpid.
 * Call with pgid=0 to clear (no foreground process group). */
void kbd_set_tty_pgrp(uint32_t pgid);

/* Return the current foreground process group ID.
 * Returns 0 if no foreground process group. */
uint32_t kbd_get_tty_pgrp(void);

/* Like kbd_read() but returns '\0' and sets *interrupted=1 if a signal
 * is pending for the current process. Returns the character otherwise. */
char kbd_read_interruptible(int *interrupted);

/* Inject an ASCII character from USB HID into the keyboard ring buffer.
 * Called from usb_hid_process_report() in interrupt context (PIT ISR path).
 * Shares the PS/2 ring buffer; zero bytes are silently dropped. */
void kbd_usb_inject(uint8_t ascii);

#endif /* AEGIS_KBD_H */
