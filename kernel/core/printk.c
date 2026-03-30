#include <stdarg.h>
#include <stdint.h>
#include "arch.h"
#include "printk.h"
#include "spinlock.h"
#include "../drivers/fb.h"

/*
 * printk — route formatted output to all available output sinks.
 *
 * Law 1: serial is written unconditionally (if serial_init was called).
 * Law 2: VGA is written only if vga_available is set by vga_init().
 *        VGA failure never silences serial.
 *
 * Supported conversions: %s %c %u %lu %x %lx %%
 * No dynamic allocation. No VLAs. Stack buffer for integer formatting only.
 *
 * printk_lock serialises output so two CPUs (or an ISR and a thread) cannot
 * interleave characters.  IRQs are saved/restored around the lock.
 */

static spinlock_t printk_lock = SPINLOCK_INIT;
static int printk_quiet = 0;  /* 1 = suppress VGA+FB in printk, serial only */

void
printk_set_quiet(int q)
{
    printk_quiet = q;
}

int
printk_get_quiet(void)
{
    return printk_quiet;
}

/* Emit a single character to all active output sinks. */
static void
emit_char(char c)
{
    /* serial_write_string with a 2-byte buf avoids a dependency on
     * serial_write_char being declared in arch.h.  Both paths below
     * go through the string variants which are already declared. */
    char buf[2];
    buf[0] = c;
    buf[1] = '\0';
    serial_write_string(buf);
    if (!printk_quiet && vga_available) {
        vga_write_string(buf);
    }
    if (!printk_quiet && fb_available) {
        fb_write_string(buf);
    }
}

/* Emit a null-terminated string to all active output sinks. */
static void
emit_string(const char *s)
{
    if (s == (void *)0) {
        serial_write_string("(null)");
        if (!printk_quiet && vga_available) {
            vga_write_string("(null)");
        }
        if (!printk_quiet && fb_available) {
            fb_write_string("(null)");
        }
        return;
    }
    serial_write_string(s);
    if (!printk_quiet && vga_available) {
        vga_write_string(s);
    }
    if (!printk_quiet && fb_available) {
        fb_write_string(s);
    }
}

/*
 * fmt_uint64 — convert a uint64_t to decimal or hex digits in buf.
 * buf must be at least 21 bytes for decimal, 17 bytes for hex.
 * Returns pointer to the first digit (within buf).
 * base must be 10 or 16.
 */
static const char *
fmt_uint64(uint64_t val, int base, char *buf, int buflen)
{
    static const char digits[] = "0123456789abcdef";
    int i = buflen - 1;

    buf[i] = '\0';
    i--;

    if (val == 0) {
        buf[i] = '0';
        return &buf[i];
    }

    while (val > 0 && i >= 0) {
        buf[i] = digits[val % (uint64_t)base];
        val /= (uint64_t)base;
        i--;
    }

    return &buf[i + 1];
}

void
printk(const char *fmt, ...)
{
    va_list ap;
    /* 24 bytes: enough for UINT64_MAX in decimal (20 digits) + NUL,
     * or for 16 hex digits + NUL.  No VLA. */
    char numbuf[24];
    irqflags_t flags = spin_lock_irqsave(&printk_lock);

    va_start(ap, fmt);

    while (*fmt != '\0') {
        if (*fmt != '%') {
            emit_char(*fmt);
            fmt++;
            continue;
        }

        /* We have a '%' — advance past it and inspect the next char. */
        fmt++;
        if (*fmt == '\0') {
            /* Trailing lone '%' — emit it and stop. */
            emit_char('%');
            break;
        }

        switch (*fmt) {
        case '%':
            emit_char('%');
            break;

        case 'c': {
            /* char is promoted to int through va_arg. */
            int c = va_arg(ap, int);
            emit_char((char)c);
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char *);
            emit_string(s);
            break;
        }

        case 'u': {
            uint32_t v = va_arg(ap, uint32_t);
            const char *p = fmt_uint64((uint64_t)v, 10, numbuf, (int)sizeof(numbuf));
            emit_string(p);
            break;
        }

        case 'x': {
            uint32_t v = va_arg(ap, uint32_t);
            const char *p = fmt_uint64((uint64_t)v, 16, numbuf, (int)sizeof(numbuf));
            emit_string(p);
            break;
        }

        case 'l': {
            /* Expect 'u' or 'x' after 'l'. */
            fmt++;
            if (*fmt == 'u') {
                uint64_t v = va_arg(ap, uint64_t);
                const char *p = fmt_uint64(v, 10, numbuf, (int)sizeof(numbuf));
                emit_string(p);
            } else if (*fmt == 'x') {
                uint64_t v = va_arg(ap, uint64_t);
                const char *p = fmt_uint64(v, 16, numbuf, (int)sizeof(numbuf));
                emit_string(p);
            } else {
                /* Unknown %l? — emit literally. */
                emit_char('%');
                emit_char('l');
                if (*fmt != '\0') {
                    emit_char(*fmt);
                } else {
                    /* fmt now points at NUL; the outer loop will exit. */
                    va_end(ap);
                    spin_unlock_irqrestore(&printk_lock, flags);
                    return;
                }
            }
            break;
        }

        default:
            /* Unknown conversion — emit the '%' and the specifier literally. */
            emit_char('%');
            emit_char(*fmt);
            break;
        }

        fmt++;
    }

    va_end(ap);
    spin_unlock_irqrestore(&printk_lock, flags);
}
