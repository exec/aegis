#ifndef AEGIS_PRINTK_H
#define AEGIS_PRINTK_H

/* printk — route formatted output to serial and VGA.
 * Supports: %s (string), %c (char), %u (uint32_t), %lu (uint64_t),
 *           %x (hex uint32_t), %lx (hex uint64_t), %% (literal %). */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* printk_set_quiet — suppress VGA+FB output, serial only.
 * Used with boot=quiet to hide kernel messages from the screen. */
void printk_set_quiet(int q);

#endif /* AEGIS_PRINTK_H */
