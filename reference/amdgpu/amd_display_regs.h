/* reference/amdgpu/amd_display_regs.h
 *
 * Key AMD display hardware constants for Aegis Phase 30.
 * For reference only — not compiled into the kernel.
 *
 * Sources: Linux kernel drivers/gpu/drm/amd/ (MIT/GPL2)
 *          Values verified against Linux 6.x amdgpu driver.
 */
#ifndef AMD_DISPLAY_REGS_H
#define AMD_DISPLAY_REGS_H

/* ── AMD PCI identity ───────────────────────────────────────────────────── */
#define AMD_VENDOR_ID            0x1002u   /* all AMD GPUs */
#define AMD_PCI_CLASS_VGA        0x0300u   /* display / VGA compatible */
#define AMD_PCI_CLASS_OTHER_DISP 0x0380u   /* display / other */

/* ── AMD iGPU (APU) PCI device IDs ─────────────────────────────────────── */
/* Raven / Picasso (Ryzen 2000/3000) */
#define AMD_DEVID_RAVEN          0x15D8u
/* Renoir / Cezanne (Ryzen 4000/5000) */
#define AMD_DEVID_RENOIR_A       0x15E7u
#define AMD_DEVID_RENOIR_B       0x1636u
#define AMD_DEVID_RENOIR_C       0x1638u
#define AMD_DEVID_RENOIR_D       0x164Cu
/* Van Gogh (Steam Deck) */
#define AMD_DEVID_VANGOGH        0x163Fu
/* Yellow Carp / Rembrandt (Ryzen 6000) */
#define AMD_DEVID_YELLOW_CARP_A  0x164Du
#define AMD_DEVID_YELLOW_CARP_B  0x1681u
/* Phoenix (Ryzen 7000 mobile, DCN 3.1) */
#define AMD_DEVID_PHOENIX        0x15BFu
/* Hawk Point (Ryzen 8000 mobile, DCN 3.1.4) */
#define AMD_DEVID_HAWK_POINT     0x15C8u

/* ── DCE (Display Core Engine) — pre-Vega hardware ─────────────────────── */
/* Scanout surface address register for CRTC0 (pipe 0).
 * MMIO byte offset from BAR0.  Read to get the active linear framebuffer PA.
 * Source: Linux dce_8_0_offset.h (DCE 8.x). */
#define AMD_DCE_D1GRPH_PRIMARY_SURFACE_ADDRESS          0x06818u  /* 32-bit PA */
#define AMD_DCE_D1GRPH_PRIMARY_SURFACE_ADDRESS_HIGH     0x06824u  /* upper 8 bits */
#define AMD_DCE_D1GRPH_PITCH                            0x06820u  /* bytes per row */

/* ── DCN (Display Core Next) — Raven and newer ──────────────────────────── */
/*
 * HUBPREQ0 register word-offsets (×4 for byte address from IP base).
 * Source: Linux dcn_1_0_offset.h (DCN 1.0, same values for DCN 2.x).
 *
 * Actual MMIO byte address = BAR0 + dc_ip_base[chip] + (word_offset × 4)
 * where dc_ip_base differs per chip (see README for per-chip values).
 */
#define AMD_DCN_HUBPREQ0_PRIMARY_SURFACE_ADDR_WO      0x057Du   /* bits [31:0]  */
#define AMD_DCN_HUBPREQ0_PRIMARY_SURFACE_ADDR_HIGH_WO 0x057Eu   /* bits [39:32] */
#define AMD_DCN_HUBPREQ0_SECONDARY_SURFACE_ADDR_WO    0x0581u
#define AMD_DCN_HUBPREQ0_SECONDARY_SURFACE_ADDR_HIGH_WO 0x0582u

/*
 * DC IP base word offsets from BAR0 (multiply by 4 for byte offset).
 * These are the segment[2] values from Linux's ip_offset.c tables.
 * Source: Linux soc15ip.h + raven1_ip_offset.c / renoir_ip_offset.c.
 */
#define AMD_DC_IP_BASE_RAVEN   0x00001CC0u  /* Raven/Picasso, DCN 1.0 */
#define AMD_DC_IP_BASE_RENOIR  0x00001CC0u  /* Renoir/Cezanne, DCN 2.x (same) */
/* Note: Yellow Carp, Phoenix, Hawk Point use different base — consult Linux sources */

/* ── Helper macro: DCN MMIO byte offset for Raven/Renoir ───────────────── */
/* offset = (dc_ip_base + word_offset) × 4 */
#define AMD_DCN_REG_BYTE_OFF(ip_base, word_off)  (((ip_base) + (word_off)) * 4u)

/* ── PAT/caching constants (x86, relevant for framebuffer mapping) ───────── */
/*
 * PAT MSR (IA32_PAT = 0x277) entry encoding (3-bit values):
 *   0x00 = UC  (strong uncacheable)
 *   0x01 = WC  (write-combining)      ← desired for framebuffer
 *   0x04 = WT  (write-through)        ← PA1 default (we override to WC)
 *   0x06 = WB  (write-back)           ← PA0 default (normal RAM)
 *   0x07 = UC- (weak uncacheable / WC-override)
 *
 * After Aegis PAT init, IA32_PAT = 0x0007010600070106:
 *   PA0 = WB (0x06) — normal RAM
 *   PA1 = WC (0x01) — framebuffer  ← changed from default WT
 *   PA2 = UC- (0x07)
 *   PA3 = UC (0x00)
 *   PA4–PA7 = repeat PA0–PA3
 *
 * PTE bit selection: index = PAT[bit7] | PCD[bit4] | PWT[bit3]
 *   PA1 (WC): PWT=1, PCD=0, PAT=0  → set bit 3 in PTE flags = VMM_FLAG_WC
 */
#define AMD_PAT_MSR               0x0277u
#define AMD_PAT_VALUE_WB_WC_UC_UC 0x0007010600070106ULL

#endif /* AMD_DISPLAY_REGS_H */
