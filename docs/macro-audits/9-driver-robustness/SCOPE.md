# Audit 9: Driver Robustness

## Priority: MEDIUM

Drivers interact with hardware that may be malicious (USB), broken, or
absent. Error handling on device timeout, malformed responses, and
resource cleanup on failure.

## Files to review

| File | LOC | Focus |
|------|-----|-------|
| `kernel/drivers/xhci.c` | 996 | USB host controller — complex state machine |
| `kernel/drivers/nvme.c` | 501 | NVMe block device — DMA, completion polling |
| `kernel/drivers/virtio_net.c` | 505 | Virtio network — ring buffer DMA |
| `kernel/drivers/fb.c` | 640 | Framebuffer text driver — MMIO mapping |
| `kernel/drivers/panic_screen.c` | 275 | Panic bluescreen + boot splash rendering |
| `kernel/drivers/usb_mouse.c` | — | USB HID mouse — input from untrusted device |
| `kernel/drivers/ramdisk.c` | 132 | RAM block device |

## Checklist

### Timeout handling
- [ ] Every hardware poll loop has a timeout (no infinite spin)
- [ ] Timeout returns error, doesn't panic
- [ ] Command timeout doesn't leave hardware in undefined state

### Malformed input
- [ ] USB HID reports validated (length, protocol)
- [ ] NVMe completion entries validated (status, CID match)
- [ ] Virtio used ring entries bounds-checked
- [ ] GPT partition entries validated (LBA ranges, entry size)

### Resource cleanup
- [ ] DMA buffers freed on driver shutdown/error
- [ ] MMIO regions not double-mapped
- [ ] Interrupt handlers deregistered on driver failure
- [ ] Transfer ring memory tracked for potential future cleanup

### DMA safety
- [ ] DMA addresses are physical (not virtual)
- [ ] Bounce buffers used where needed (IOMMU absent)
- [ ] No kernel memory corruption via rogue DMA writes

## Output format

Same as Audit 1.
