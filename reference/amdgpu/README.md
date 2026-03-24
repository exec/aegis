# AMD GPU / Display Core Reference

Reference material for Aegis Phase 30: AMD Display Core (DC) driver.

## Current approach (Phase 29)

The Aegis framebuffer driver (`kernel/drivers/fb.c`) uses the linear framebuffer
that UEFI + GRUB set up via GOP (Graphics Output Protocol). GRUB passes its
physical address via the multiboot2 type-8 tag. The kernel maps it with PAT
write-combining and renders text using the embedded 8×16 font.

**Requirement:** Boot Aegis in UEFI mode. Modern AMD APUs (Ryzen 4000+) removed
legacy VBE support; the multiboot2 framebuffer tag is absent in BIOS/legacy mode.

## Phase 30: AMD Display Core

A direct AMD hardware driver would:
1. Enumerate the AMD iGPU via PCI (vendor 0x1002, class 0x0300 or 0x0380)
2. Map the GPU MMIO BAR (BAR0 for APU, 512KB–4MB depending on generation)
3. For DCE hardware (pre-Raven): read `D1GRPH_PRIMARY_SURFACE_ADDRESS` (offset 0x6818)
4. For DCN hardware (Raven / Renoir / Cezanne / Phoenix):
   - Read `HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS` (DCN1: BAR0 offset 0x057d×4 + DC IP base)
   - Read `HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH` for the upper 32 bits
5. Those registers give the currently-active scanout physical address — usable without
   re-initializing the display pipeline (UEFI already set it up)

This avoids re-implementing clock PLL setup, VBIOS parsing, EDID detection, and the
full AMD DC (Display Core) state machine — saving tens of thousands of lines.

## Key AMD APU PCI Device IDs

| Chip       | Device ID | Ryzen generation     |
|------------|-----------|----------------------|
| Kaveri     | 0x130x    | A-series (2014)      |
| Kabini     | 0x983x    | Athlon (2013)        |
| Carrizo    | 0x987x    | A-series (2015)      |
| Bristol Rd | 0x98xx    | A-series (2016)      |
| Raven      | 0x15D8    | Ryzen 2000 APU       |
| Picasso    | 0x15D8    | Ryzen 3000 APU       |
| Renoir     | 0x1636/0x1638/0x15E7/0x164C | Ryzen 4000/5000 |
| Cezanne    | 0x1638    | Ryzen 5000 APU       |
| Van Gogh   | 0x163F    | Steam Deck           |
| Yellow Carp (Rembrandt) | 0x164D/0x1681 | Ryzen 6000 |
| Cyan Skillfish | 0x13DB–0x143F | Ryzen 5000 embedded |
| Phoenix    | 0x15BF    | Ryzen 7000 mobile    |
| Hawk Point | 0x15C8    | Ryzen 8000 mobile    |

All AMD GPUs: vendor_id = 0x1002, class = 0x0300 (VGA) or 0x0380 (Other display).

## DCN register address calculation

The DCN (Display Core Next) IP base address varies per chip family. In Linux:

    actual_reg_addr = mmio_base + ip_offset[DC_HWIP][instance][BASE_IDX] + mmREG * 4

For Raven (DCN 1.0), DC_BASE[instance=0][BASE_IDX=2] = 0x00001CC0.
So: HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS MMIO byte offset = (0x00001CC0 + 0x057d) × 4

But this offset differs for Renoir (DCN 2.1), Cezanne (DCN 2.1.01), Phoenix (DCN 3.x).
Consult `drivers/gpu/drm/amd/<chip>/ip_offset.c` for the table.

## References

- Linux kernel: `drivers/gpu/drm/amd/`
- AMD Display Core: `drivers/gpu/drm/amd/display/dc/`
- HUBPREQ registers: `include/asic_reg/dcn/dcn_1_0_offset.h`
- DCE registers: `include/asic_reg/dce/dce_8_0_offset.h`
- virtio-gpu (Phase 30+): see `reference/virtio-gpu/`
