/* kernel/drivers/fb.h — Linear framebuffer character terminal.
 *
 * Requires arch_mm_init() to have run (multiboot2 tag parsed) and
 * vmm_init() + kva_init() to have run (needed for page mapping).
 *
 * fb_init() maps the hardware framebuffer into kernel KVA and initialises
 * the embedded 8×16 font renderer.  After fb_init(), fb_putchar() may be
 * called from printk() on every character.
 */
#ifndef FB_H
#define FB_H

/* fb_init — detect framebuffer from arch_get_fb_info(), map it, clear screen.
 * No-op (silent) if no framebuffer was provided by GRUB. */
void fb_init(void);

/* fb_putchar — render one ASCII character to the framebuffer terminal.
 * Only callable after fb_init().  Ignores the call if fb_available == 0. */
void fb_putchar(char c);

/* fb_write_string — render a NUL-terminated string. */
void fb_write_string(const char *s);

/* fb_check_amd — log a diagnostic if an AMD iGPU is present but
 * fb_available == 0 (no UEFI GOP framebuffer was provided by GRUB).
 * Must be called AFTER pcie_init() so the device table is populated.
 * No-op if fb_available == 1 (framebuffer already working) or no AMD GPU. */
void fb_check_amd(void);

/* fb_available — set to 1 by fb_init() on success; 0 otherwise.
 * Checked by printk() before calling fb_write_string(). */
extern int fb_available;

#endif /* FB_H */
