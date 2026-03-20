#ifndef AEGIS_PRINTK_H
#define AEGIS_PRINTK_H

/* printk — route formatted output to serial and VGA.
 * Supports: %s (string), %c (char), %u (uint32_t), %lu (uint64_t),
 *           %x (hex uint32_t), %lx (hex uint64_t), %% (literal %). */
void printk(const char *fmt, ...);

#endif /* AEGIS_PRINTK_H */
