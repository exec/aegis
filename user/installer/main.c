/* user/installer/main.c — Aegis text-mode installer
 *
 * Partitions NVMe, copies rootfs from ramdisk, installs GRUB.
 * Requires CAP_KIND_DISK_ADMIN (granted in execve baseline).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

/* ── Syscall wrappers ──────────────────────────────────────────────── */

typedef struct {
    char     name[16];
    unsigned long long block_count;
    unsigned int       block_size;
    unsigned int       _pad;
} blkdev_info_t;

static long blkdev_list(blkdev_info_t *buf, unsigned long bufsize)
{
    return syscall(510, buf, bufsize);
}

static long blkdev_io(const char *name, unsigned long long lba,
                      unsigned long long count, void *buf, int wr)
{
    return syscall(511, name, lba, count, buf, (unsigned long)wr);
}

static long gpt_rescan(const char *name)
{
    return syscall(512, name);
}

/* ── CRC32 ─────────────────────────────────────────────────────────── */

static unsigned int crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void)
{
    unsigned int i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320U : 0U);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static unsigned int crc32(const void *data, unsigned int len)
{
    if (!crc32_ready) crc32_init();
    const unsigned char *p = data;
    unsigned int crc = 0xFFFFFFFF;
    unsigned int i;
    for (i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ── GPT structures ────────────────────────────────────────────────── */

/* Protective MBR — one partition spanning the disk */
static void write_protective_mbr(unsigned char *mbr, unsigned long long disk_sectors)
{
    memset(mbr, 0, 512);
    /* Partition entry 1 at offset 446 */
    mbr[446] = 0x00;        /* status: not bootable */
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00;  /* CHS start */
    mbr[450] = 0xEE;        /* type: GPT protective */
    mbr[451] = 0xFF; mbr[452] = 0xFF; mbr[453] = 0xFF;  /* CHS end */
    /* LBA start = 1 */
    mbr[454] = 0x01; mbr[455] = 0; mbr[456] = 0; mbr[457] = 0;
    /* LBA size — cap at 0xFFFFFFFF */
    unsigned int sz = (disk_sectors - 1 > 0xFFFFFFFFULL)
                      ? 0xFFFFFFFF : (unsigned int)(disk_sectors - 1);
    memcpy(&mbr[458], &sz, 4);
    /* Boot signature */
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

/* GUID: A3618F24-0C76-4B3D-0001-000000000000 (Aegis root) */
static const unsigned char AEGIS_ROOT_GUID[16] = {
    0x24,0x8F,0x61,0xA3, 0x76,0x0C, 0x3D,0x4B,
    0x00,0x01, 0x00,0x00,0x00,0x00,0x00,0x00
};
/* GUID: 21686148-6449-6E6F-744E-656564454649 (BIOS Boot) */
static const unsigned char BIOS_BOOT_GUID[16] = {
    0x48,0x61,0x68,0x21, 0x49,0x64, 0x6F,0x6E,
    0x74,0x4E, 0x65,0x65,0x64,0x45,0x46,0x49
};

/* Write GPT to disk. Returns 0 on success. */
static int write_gpt(const char *devname, unsigned long long disk_blocks)
{
    unsigned char sector[512];
    unsigned char entries[128 * 128];  /* 128 entries × 128 bytes = 16KB */
    unsigned long long last_lba = disk_blocks - 1;

    /* Partition 1: BIOS Boot — LBA 34 to 2047 (1 MB) */
    unsigned long long p1_start = 34;
    unsigned long long p1_end   = 2047;

    /* Partition 2: Aegis Root — LBA 2048 to last_lba - 33 */
    unsigned long long p2_start = 2048;
    unsigned long long p2_end   = last_lba - 33;

    /* ── Write protective MBR (LBA 0) ── */
    write_protective_mbr(sector, disk_blocks);
    if (blkdev_io(devname, 0, 1, sector, 1) < 0) return -1;

    /* ── Build partition entries ── */
    memset(entries, 0, sizeof(entries));

    /* Entry 0: BIOS Boot */
    memcpy(&entries[0], BIOS_BOOT_GUID, 16);         /* type GUID */
    /* Unique GUID: just use a fixed value for now */
    entries[16] = 0x01; entries[17] = 0x02; entries[18] = 0x03; entries[19] = 0x04;
    memcpy(&entries[32], &p1_start, 8);               /* start LBA */
    memcpy(&entries[40], &p1_end, 8);                 /* end LBA */

    /* Entry 1: Aegis Root */
    memcpy(&entries[128], AEGIS_ROOT_GUID, 16);       /* type GUID */
    entries[128+16] = 0x05; entries[128+17] = 0x06;
    entries[128+18] = 0x07; entries[128+19] = 0x08;
    memcpy(&entries[128+32], &p2_start, 8);            /* start LBA */
    memcpy(&entries[128+40], &p2_end, 8);              /* end LBA */

    unsigned int entry_crc = crc32(entries, 128 * 128);

    /* ── Build primary GPT header (LBA 1) ── */
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);                    /* signature */
    unsigned int rev = 0x00010000; memcpy(&sector[8], &rev, 4);  /* revision */
    unsigned int hsz = 92; memcpy(&sector[12], &hsz, 4);         /* header size */
    /* header CRC at offset 16 — fill later */
    unsigned long long my_lba = 1; memcpy(&sector[24], &my_lba, 8);
    unsigned long long alt_lba = last_lba; memcpy(&sector[32], &alt_lba, 8);
    unsigned long long first_usable = 34; memcpy(&sector[40], &first_usable, 8);
    unsigned long long last_usable = last_lba - 33; memcpy(&sector[48], &last_usable, 8);
    /* disk GUID at offset 56 — 16 bytes, fixed */
    sector[56] = 0xAE; sector[57] = 0x61; sector[58] = 0x15; sector[59] = 0x00;
    unsigned long long entry_lba = 2; memcpy(&sector[72], &entry_lba, 8);
    unsigned int nentries = 128; memcpy(&sector[80], &nentries, 4);
    unsigned int entry_sz = 128; memcpy(&sector[84], &entry_sz, 4);
    memcpy(&sector[88], &entry_crc, 4);               /* partition array CRC */
    /* Compute header CRC */
    unsigned int hcrc = crc32(sector, 92);
    memcpy(&sector[16], &hcrc, 4);

    if (blkdev_io(devname, 1, 1, sector, 1) < 0) return -1;

    /* ── Write partition entries (LBAs 2-33) ── */
    unsigned long long lba;
    for (lba = 0; lba < 32; lba++) {
        if (blkdev_io(devname, 2 + lba, 1, entries + lba * 512, 1) < 0)
            return -1;
    }

    /* ── Backup partition entries (last 32 LBAs before backup header) ── */
    for (lba = 0; lba < 32; lba++) {
        if (blkdev_io(devname, last_lba - 32 + lba, 1, entries + lba * 512, 1) < 0)
            return -1;
    }

    /* ── Backup GPT header (last LBA) ── */
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    memcpy(&sector[8], &rev, 4);
    memcpy(&sector[12], &hsz, 4);
    my_lba = last_lba; memcpy(&sector[24], &my_lba, 8);
    alt_lba = 1; memcpy(&sector[32], &alt_lba, 8);
    memcpy(&sector[40], &first_usable, 8);
    memcpy(&sector[48], &last_usable, 8);
    sector[56] = 0xAE; sector[57] = 0x61; sector[58] = 0x15; sector[59] = 0x00;
    entry_lba = last_lba - 32; memcpy(&sector[72], &entry_lba, 8);
    memcpy(&sector[80], &nentries, 4);
    memcpy(&sector[84], &entry_sz, 4);
    memcpy(&sector[88], &entry_crc, 4);
    memset(&sector[16], 0, 4);  /* zero header CRC field before computing */
    hcrc = crc32(sector, 92);
    memcpy(&sector[16], &hcrc, 4);

    if (blkdev_io(devname, last_lba, 1, sector, 1) < 0) return -1;

    return 0;
}

/* ── Rootfs copy ───────────────────────────────────────────────────── */

static int copy_rootfs(const char *dst_dev, unsigned long long dst_blocks)
{
    /* Find ramdisk0 size */
    blkdev_info_t devs[8];
    int n = (int)blkdev_list(devs, sizeof(devs));
    unsigned long long src_blocks = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk0") == 0) {
            src_blocks = devs[i].block_count;
            break;
        }
    }
    if (src_blocks == 0) {
        printf("ERROR: ramdisk0 not found\n");
        return -1;
    }
    if (src_blocks > dst_blocks) {
        printf("ERROR: rootfs (%llu blocks) too large for partition (%llu blocks)\n",
               src_blocks, dst_blocks);
        return -1;
    }

    /* Copy 8 sectors at a time */
    unsigned char buf[4096];
    unsigned long long lba;
    unsigned long long total = src_blocks;
    unsigned long long last_pct = 0;
    for (lba = 0; lba < total; lba += 8) {
        unsigned long long chunk = total - lba;
        if (chunk > 8) chunk = 8;
        if (blkdev_io("ramdisk0", lba, chunk, buf, 0) < 0) return -1;
        if (blkdev_io(dst_dev, lba, chunk, buf, 1) < 0) return -1;
        unsigned long long pct = (lba + chunk) * 100 / total;
        if (pct != last_pct && pct % 10 == 0) {
            printf("  %llu%%\n", pct);
            last_pct = pct;
        }
    }
    return 0;
}

/* ── GRUB installer ────────────────────────────────────────────────── */

static int read_file(const char *path, unsigned char *buf, int maxlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    while (total < maxlen) {
        int n = (int)read(fd, buf + total, (size_t)(maxlen - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    return total;
}

static int install_grub(const char *devname)
{
    unsigned char boot_img[512];
    unsigned char core_buf[65536];  /* core.img, typically 30-60KB */

    /* Read boot.img from live filesystem */
    int boot_sz = read_file("/boot/grub/boot.img", boot_img, 512);
    if (boot_sz < 440) {
        printf("ERROR: cannot read /boot/grub/boot.img (%d bytes)\n", boot_sz);
        return -1;
    }

    /* Read core.img */
    int core_sz = read_file("/boot/grub/core.img", core_buf, (int)sizeof(core_buf));
    if (core_sz <= 0) {
        printf("ERROR: cannot read /boot/grub/core.img\n");
        return -1;
    }

    /* Read current MBR (preserve GPT protective entry at offset 446) */
    unsigned char mbr[512];
    if (blkdev_io(devname, 0, 1, mbr, 0) < 0) return -1;

    /* Overwrite MBR bootstrap (bytes 0-439) with boot.img */
    memcpy(mbr, boot_img, 440);

    /* Write MBR back */
    if (blkdev_io(devname, 0, 1, mbr, 1) < 0) return -1;

    /* Write core.img to BIOS Boot Partition (LBAs 34-2047) */
    unsigned long long core_sectors = ((unsigned long long)core_sz + 511) / 512;
    unsigned long long lba;
    for (lba = 0; lba < core_sectors; lba++) {
        unsigned char sector[512];
        memset(sector, 0, 512);
        unsigned long long off = lba * 512;
        unsigned long long remain = (unsigned long long)core_sz - off;
        if (remain > 512) remain = 512;
        memcpy(sector, core_buf + off, (size_t)remain);
        if (blkdev_io(devname, 34 + lba, 1, sector, 1) < 0) return -1;
    }

    return 0;
}

/* ── Write installed grub.cfg ──────────────────────────────────────── */

static int write_grub_cfg(void)
{
    /* Write grub.cfg to the installed ext2 partition.
     * After rootfs copy + gpt_rescan, the NVMe root is mounted as the active
     * ext2. We can write to it via normal file I/O since ext2 is writable. */
    int fd = open("/boot/grub/grub.cfg", O_WRONLY | O_CREAT);
    if (fd < 0) {
        /* The rootfs copy didn't include /boot/grub/grub.cfg with the installed
         * config — we need to create it. But ext2 is currently the ramdisk.
         * After the copy, the NVMe partition IS the same data as ramdisk.
         * We need to remount... actually, ext2 is still mounted on ramdisk.
         * Writing to /boot/grub/grub.cfg writes to the ramdisk ext2, not NVMe.
         *
         * The grub.cfg must be written to the NVMe partition directly via
         * block I/O after the copy. We'll write it by modifying the ext2
         * filesystem on the NVMe partition.
         *
         * Simplest approach: write the grub.cfg to the ramdisk BEFORE copying
         * to NVMe. Then the copy includes it. */
        printf("ERROR: cannot create /boot/grub/grub.cfg\n");
        return -1;
    }
    const char *cfg =
        "set timeout=3\n"
        "set default=0\n"
        "insmod all_video\n"
        "insmod gfxterm\n"
        "set gfxmode=1024x768x32,auto\n"
        "terminal_input console\n"
        "terminal_output gfxterm\n"
        "\n"
        "menuentry \"Aegis\" {\n"
        "    set gfxpayload=keep\n"
        "    set root=(hd0,gpt2)\n"
        "    multiboot2 /boot/aegis.elf\n"
        "    boot\n"
        "}\n";
    write(fd, cfg, strlen(cfg));
    close(fd);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\n=== Aegis Installer ===\n\n");
    printf("This will install Aegis to your NVMe disk.\n");
    printf("WARNING: All data on the disk will be destroyed!\n\n");

    /* Enumerate block devices */
    blkdev_info_t devs[8];
    int ndevs = (int)blkdev_list(devs, sizeof(devs));
    if (ndevs <= 0) {
        printf("ERROR: cannot enumerate block devices\n");
        return 1;
    }

    /* Find nvme0 (skip ramdisk, partitions) */
    int target = -1;
    printf("Available disks:\n");
    int i;
    for (i = 0; i < ndevs; i++) {
        /* Skip ramdisk and partition devices (contain 'p' after a digit) */
        if (strncmp(devs[i].name, "ramdisk", 7) == 0) continue;
        if (strchr(devs[i].name, 'p') != NULL) continue;
        printf("  %s: %llu sectors (%llu MB)\n",
               devs[i].name, devs[i].block_count,
               devs[i].block_count * devs[i].block_size / (1024*1024));
        target = i;
    }

    if (target < 0) {
        printf("\nNo suitable disk found.\n");
        return 1;
    }

    printf("\nInstall to %s? [y/N] ", devs[target].name);
    fflush(stdout);
    char ans[8];
    if (fgets(ans, sizeof(ans), stdin) == NULL || (ans[0] != 'y' && ans[0] != 'Y')) {
        printf("Aborted.\n");
        return 0;
    }

    const char *devname = devs[target].name;
    unsigned long long disk_blocks = devs[target].block_count;

    /* 1. Write installed grub.cfg to ramdisk ext2 BEFORE copying */
    printf("\nWriting grub.cfg... ");
    fflush(stdout);
    if (write_grub_cfg() < 0) return 1;
    printf("done\n");

    /* 2. Create GPT */
    printf("Creating GPT partition table... ");
    fflush(stdout);
    if (write_gpt(devname, disk_blocks) < 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("done\n");

    /* 3. Rescan partitions */
    printf("Rescanning partitions... ");
    fflush(stdout);
    int nparts = (int)gpt_rescan(devname);
    if (nparts <= 0) {
        printf("FAILED (found %d partitions)\n", nparts);
        return 1;
    }
    printf("done (%d partition(s))\n", nparts);

    /* Find the Aegis root partition name */
    ndevs = (int)blkdev_list(devs, sizeof(devs));
    char root_part[16] = "";
    unsigned long long root_blocks = 0;
    for (i = 0; i < ndevs; i++) {
        /* Look for devname + "p" + digit */
        if (strncmp(devs[i].name, devname, strlen(devname)) == 0 &&
            devs[i].name[strlen(devname)] == 'p') {
            strcpy(root_part, devs[i].name);
            root_blocks = devs[i].block_count;
            break;
        }
    }
    if (root_part[0] == '\0') {
        printf("ERROR: root partition not found after rescan\n");
        return 1;
    }
    printf("  Root partition: %s (%llu MB)\n",
           root_part, root_blocks * 512 / (1024*1024));

    /* 4. Copy rootfs */
    printf("Copying root filesystem...\n");
    if (copy_rootfs(root_part, root_blocks) < 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("  done\n");

    /* 5. Install GRUB */
    printf("Installing GRUB bootloader... ");
    fflush(stdout);
    if (install_grub(devname) < 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("done\n");

    /* 6. Sync */
    printf("Syncing disk... ");
    fflush(stdout);
    sync();
    printf("done\n");

    printf("\n=== Installation complete! ===\n");
    printf("Remove the ISO and reboot to start Aegis from disk.\n\n");

    return 0;
}
