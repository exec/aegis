/* copy.c — block device enumeration + rootfs/ESP copy (libinstall) */
#include "libinstall.h"
#include "syscalls.h"
#include <string.h>

/* ESP layout — same constants as gpt.c. Kept local here so copy.c
 * doesn't cross-include gpt.c internals. */
#define ESP_START   2048ULL
#define ESP_SECTORS 65536ULL

static void report_err(install_progress_t *p, const char *msg)
{
    if (p && p->on_error)
        p->on_error(msg, p->ctx);
}

/* ── Public: install_list_blkdevs ───────────────────────────────────── */

int install_list_blkdevs(install_blkdev_t *out, int max)
{
    long n = li_blkdev_list(out,
                            (unsigned long)(sizeof(install_blkdev_t) * (unsigned)max));
    if (n < 0)
        return 0;
    return (int)n;
}

/* ── Block copy helper (file-local) ─────────────────────────────────── */

static int copy_blocks_internal(const char *src_dev, const char *dst_dev,
                                uint64_t count, install_progress_t *p)
{
    static unsigned char buf[4096];
    uint64_t lba;
    int last_pct = -1;
    for (lba = 0; lba < count; lba += 8) {
        uint64_t chunk = count - lba;
        if (chunk > 8) chunk = 8;
        if (li_blkdev_io(src_dev, lba, chunk, buf, 0) < 0) {
            report_err(p, "block read failed");
            return -1;
        }
        if (li_blkdev_io(dst_dev, lba, chunk, buf, 1) < 0) {
            report_err(p, "block write failed");
            return -1;
        }
        int pct = (int)((lba + chunk) * 100 / count);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress)
                p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    /* Ensure we emit a 100% callback even if the loop didn't hit a
     * multiple of 10 on its final iteration. */
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/* ── Public: install_copy_esp ───────────────────────────────────────── */

int install_copy_esp(const char *devname, install_progress_t *p)
{
    if (p && p->on_step)
        p->on_step("Installing EFI bootloader", p->ctx);

    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    uint64_t esp_blocks = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk1") == 0) {
            esp_blocks = devs[i].block_count;
            break;
        }
    }
    if (esp_blocks == 0) {
        report_err(p, "ramdisk1 (ESP image) not found");
        return -1;
    }
    if (esp_blocks > ESP_SECTORS) esp_blocks = ESP_SECTORS;

    /* Copy ramdisk1 -> devname at the ESP offset.
     * We cannot reuse copy_blocks_internal because the destination
     * LBA is offset (ESP_START), not zero. */
    static unsigned char buf[4096];
    uint64_t lba;
    int last_pct = -1;
    for (lba = 0; lba < esp_blocks; lba += 8) {
        uint64_t chunk = esp_blocks - lba;
        if (chunk > 8) chunk = 8;
        if (li_blkdev_io("ramdisk1", lba, chunk, buf, 0) < 0) {
            report_err(p, "ESP read failed");
            return -1;
        }
        if (li_blkdev_io(devname, ESP_START + lba, chunk, buf, 1) < 0) {
            report_err(p, "ESP write failed");
            return -1;
        }
        int pct = (int)((lba + chunk) * 100 / esp_blocks);
        if (pct != last_pct && pct % 10 == 0) {
            if (p && p->on_progress)
                p->on_progress(pct, p->ctx);
            last_pct = pct;
        }
    }
    if (p && p->on_progress && last_pct != 100)
        p->on_progress(100, p->ctx);
    return 0;
}

/* ── Public: install_copy_rootfs ────────────────────────────────────── */

int install_copy_rootfs(const char *dst_dev, uint64_t dst_blocks,
                        install_progress_t *p)
{
    if (p && p->on_step)
        p->on_step("Copying root filesystem", p->ctx);

    install_blkdev_t devs[8];
    int n = install_list_blkdevs(devs, 8);
    uint64_t src_blocks = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk0") == 0) {
            src_blocks = devs[i].block_count;
            break;
        }
    }
    if (src_blocks == 0) {
        report_err(p, "ramdisk0 not found");
        return -1;
    }
    if (src_blocks > dst_blocks) {
        report_err(p, "rootfs larger than target partition");
        return -1;
    }
    return copy_blocks_internal("ramdisk0", dst_dev, src_blocks, p);
}
