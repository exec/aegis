#ifndef PRINTK_H
#define PRINTK_H

/* Write a null-terminated string to all available outputs.
 * Routes to serial (always) and VGA (if initialised).
 * Not safe to call before arch_init(). */
void printk(const char *s);

#endif
