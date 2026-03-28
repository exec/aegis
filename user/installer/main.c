/* user/installer/main.c — Aegis text-mode installer
 *
 * Partitions NVMe with UEFI GPT, copies rootfs from ramdisk, installs
 * EFI GRUB bootloader, sets up user account. Requires CAP_KIND_DISK_ADMIN.
 *
 * Partition layout:
 *   1. EFI System Partition (32 MB, FAT — pre-built esp.img from rootfs)
 *   2. Aegis Root (rest of disk, ext2 — copied from ramdisk)
 *
 * The ESP image (containing /EFI/BOOT/BOOTX64.EFI + grub.cfg) is pre-built
 * at make time and embedded in rootfs.img at /boot/esp.img. The installer
 * copies it block-for-block to partition 1. No FAT formatting needed at
 * runtime.
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

static void write_protective_mbr(unsigned char *mbr, unsigned long long disk_sectors)
{
    memset(mbr, 0, 512);
    mbr[446] = 0x00;
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00;
    mbr[450] = 0xEE;
    mbr[451] = 0xFF; mbr[452] = 0xFF; mbr[453] = 0xFF;
    mbr[454] = 0x01; mbr[455] = 0; mbr[456] = 0; mbr[457] = 0;
    unsigned int sz = (disk_sectors - 1 > 0xFFFFFFFFULL)
                      ? 0xFFFFFFFF : (unsigned int)(disk_sectors - 1);
    memcpy(&mbr[458], &sz, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

/* GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B (EFI System Partition) */
static const unsigned char ESP_GUID[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
/* GUID: A3618F24-0C76-4B3D-0001-000000000000 (Aegis root) */
static const unsigned char AEGIS_ROOT_GUID[16] = {
    0x24,0x8F,0x61,0xA3, 0x76,0x0C, 0x3D,0x4B,
    0x00,0x01, 0x00,0x00,0x00,0x00,0x00,0x00
};

/* ESP: 32 MB = 65536 sectors starting at LBA 2048 */
#define ESP_START   2048ULL
#define ESP_SECTORS 65536ULL
#define ESP_END     (ESP_START + ESP_SECTORS - 1)
/* Root: starts after ESP */
#define ROOT_START  (ESP_END + 1)

static int write_gpt(const char *devname, unsigned long long disk_blocks)
{
    static unsigned char sector[512];
    static unsigned char entries[128 * 128];
    unsigned long long last_lba = disk_blocks - 1;
    unsigned long long root_end = last_lba - 33;

    if (root_end <= ROOT_START) {
        printf("ERROR: disk too small (need >%llu sectors)\n", ROOT_START + 34);
        return -1;
    }

    write_protective_mbr(sector, disk_blocks);
    if (blkdev_io(devname, 0, 1, sector, 1) < 0) return -1;

    memset(entries, 0, sizeof(entries));

    /* Entry 0: EFI System Partition */
    memcpy(&entries[0], ESP_GUID, 16);
    entries[16] = 0x01; entries[17] = 0x02; entries[18] = 0x03; entries[19] = 0x04;
    {
        unsigned long long s = ESP_START, e = ESP_END;
        memcpy(&entries[32], &s, 8);
        memcpy(&entries[40], &e, 8);
    }

    /* Entry 1: Aegis Root */
    memcpy(&entries[128], AEGIS_ROOT_GUID, 16);
    entries[128+16] = 0x05; entries[128+17] = 0x06;
    entries[128+18] = 0x07; entries[128+19] = 0x08;
    {
        unsigned long long s = ROOT_START, e = root_end;
        memcpy(&entries[128+32], &s, 8);
        memcpy(&entries[128+40], &e, 8);
    }

    unsigned int entry_crc = crc32(entries, 128 * 128);

    /* Primary GPT header (LBA 1) */
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    unsigned int rev = 0x00010000; memcpy(&sector[8], &rev, 4);
    unsigned int hsz = 92; memcpy(&sector[12], &hsz, 4);
    {
        unsigned long long v;
        v = 1; memcpy(&sector[24], &v, 8);           /* my_lba */
        v = last_lba; memcpy(&sector[32], &v, 8);    /* alt_lba */
        v = 34; memcpy(&sector[40], &v, 8);          /* first_usable */
        v = last_lba - 33; memcpy(&sector[48], &v, 8); /* last_usable */
    }
    sector[56] = 0xAE; sector[57] = 0x61; sector[58] = 0x15; sector[59] = 0x00;
    {
        unsigned long long v = 2;
        memcpy(&sector[72], &v, 8);                   /* partition_entry_lba */
    }
    unsigned int nentries = 128; memcpy(&sector[80], &nentries, 4);
    unsigned int entry_sz = 128; memcpy(&sector[84], &entry_sz, 4);
    memcpy(&sector[88], &entry_crc, 4);
    unsigned int hcrc = crc32(sector, 92);
    memcpy(&sector[16], &hcrc, 4);
    if (blkdev_io(devname, 1, 1, sector, 1) < 0) return -1;

    /* Write partition entries (LBAs 2-33) */
    unsigned long long lba;
    for (lba = 0; lba < 32; lba++)
        if (blkdev_io(devname, 2 + lba, 1, entries + lba * 512, 1) < 0) return -1;

    /* Backup entries + header */
    for (lba = 0; lba < 32; lba++)
        if (blkdev_io(devname, last_lba - 32 + lba, 1, entries + lba * 512, 1) < 0) return -1;

    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    memcpy(&sector[8], &rev, 4);
    memcpy(&sector[12], &hsz, 4);
    {
        unsigned long long v;
        v = last_lba; memcpy(&sector[24], &v, 8);
        v = 1; memcpy(&sector[32], &v, 8);
        v = 34; memcpy(&sector[40], &v, 8);
        v = last_lba - 33; memcpy(&sector[48], &v, 8);
    }
    sector[56] = 0xAE; sector[57] = 0x61; sector[58] = 0x15; sector[59] = 0x00;
    {
        unsigned long long v = last_lba - 32;
        memcpy(&sector[72], &v, 8);
    }
    memcpy(&sector[80], &nentries, 4);
    memcpy(&sector[84], &entry_sz, 4);
    memcpy(&sector[88], &entry_crc, 4);
    memset(&sector[16], 0, 4);
    hcrc = crc32(sector, 92);
    memcpy(&sector[16], &hcrc, 4);
    if (blkdev_io(devname, last_lba, 1, sector, 1) < 0) return -1;

    return 0;
}

/* ── Block copy helper ─────────────────────────────────────────────── */

static int copy_blocks(const char *src_dev, const char *dst_dev,
                       unsigned long long count, const char *label)
{
    static unsigned char buf[4096];
    unsigned long long lba;
    unsigned long long last_pct = 0;
    for (lba = 0; lba < count; lba += 8) {
        unsigned long long chunk = count - lba;
        if (chunk > 8) chunk = 8;
        if (blkdev_io(src_dev, lba, chunk, buf, 0) < 0) return -1;
        if (blkdev_io(dst_dev, lba, chunk, buf, 1) < 0) return -1;
        unsigned long long pct = (lba + chunk) * 100 / count;
        if (pct != last_pct && pct % 10 == 0) {
            printf("  %s: %llu%%\n", label, pct);
            last_pct = pct;
        }
    }
    return 0;
}

/* ── ESP installation ──────────────────────────────────────────────── */

static int install_esp(const char *devname)
{
    /* The ESP image is loaded by GRUB as the second multiboot2 module
     * and registered as blkdev "ramdisk1" by the kernel. We copy it
     * block-for-block to the ESP partition on the target disk.
     *
     * The ESP partition is NOT registered by gpt_scan (no Aegis GUID prefix),
     * so we write directly to the parent device at the ESP's LBA offset. */
    blkdev_info_t devs[8];
    int n = (int)blkdev_list(devs, sizeof(devs));
    unsigned long long esp_blocks = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, "ramdisk1") == 0) {
            esp_blocks = devs[i].block_count;
            break;
        }
    }
    if (esp_blocks == 0) {
        printf("ERROR: ramdisk1 (ESP image) not found\n");
        return -1;
    }
    if (esp_blocks > ESP_SECTORS) esp_blocks = ESP_SECTORS;

    static unsigned char buf[4096];
    unsigned long long lba;
    for (lba = 0; lba < esp_blocks; lba += 8) {
        unsigned long long chunk = esp_blocks - lba;
        if (chunk > 8) chunk = 8;
        if (blkdev_io("ramdisk1", lba, chunk, buf, 0) < 0) return -1;
        if (blkdev_io(devname, ESP_START + lba, chunk, buf, 1) < 0) return -1;
    }
    return 0;
}

/* ── Rootfs copy ───────────────────────────────────────────────────── */

static int copy_rootfs(const char *dst_dev, unsigned long long dst_blocks)
{
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
        printf("ERROR: rootfs (%llu blocks) > partition (%llu blocks)\n",
               src_blocks, dst_blocks);
        return -1;
    }
    return copy_blocks("ramdisk0", dst_dev, src_blocks, "rootfs");
}

/* ── Write installed grub.cfg ──────────────────────────────────────── */

static int write_grub_cfg(void)
{
    int fd = open("/boot/grub/grub.cfg", O_WRONLY | O_CREAT);
    if (fd < 0) {
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

/* ── Strip test binaries from installed rootfs ─────────────────────── */

static void strip_test_binaries(void)
{
    /* Remove test binaries that shouldn't be on an installed system */
    unlink("/bin/thread_test");
    unlink("/bin/mmap_test");
    unlink("/bin/proc_test");
    unlink("/bin/pty_test");
    unlink("/bin/dynlink_test");
}

/* ── User account setup ────────────────────────────────────────────── */

static int setup_user(void)
{
    char username[64] = "root";
    char password[64] = "";
    char confirm[64] = "";

    printf("\n--- User Account Setup ---\n");
    printf("Username [root]: ");
    fflush(stdout);
    if (fgets(username, sizeof(username), stdin) != NULL) {
        /* Strip newline */
        int len = (int)strlen(username);
        if (len > 0 && username[len-1] == '\n') username[len-1] = '\0';
        if (username[0] == '\0') strcpy(username, "root");
    }

    printf("Password: ");
    fflush(stdout);
    if (fgets(password, sizeof(password), stdin) == NULL || password[0] == '\n') {
        printf("ERROR: password cannot be empty\n");
        return -1;
    }
    {
        int len = (int)strlen(password);
        if (len > 0 && password[len-1] == '\n') password[len-1] = '\0';
    }

    printf("Confirm password: ");
    fflush(stdout);
    if (fgets(confirm, sizeof(confirm), stdin) == NULL) {
        printf("ERROR: password confirmation failed\n");
        return -1;
    }
    {
        int len = (int)strlen(confirm);
        if (len > 0 && confirm[len-1] == '\n') confirm[len-1] = '\0';
    }

    if (strcmp(password, confirm) != 0) {
        printf("ERROR: passwords do not match\n");
        return -1;
    }

    /* Write /etc/passwd — use the entered username, shell = /bin/oksh */
    {
        int fd = open("/etc/passwd", O_WRONLY | O_CREAT);
        if (fd < 0) { printf("ERROR: cannot write /etc/passwd\n"); return -1; }
        char line[256];
        int n = snprintf(line, sizeof(line), "%s:x:0:0:%s:/root:/bin/oksh\n",
                         username, username);
        write(fd, line, (size_t)n);
        close(fd);
    }

    /* Write /etc/shadow — store password in plaintext hash format.
     * A real system would use SHA-512 crypt, but we don't have crypt() in
     * musl-static. The login binary compares the shadow hash field against
     * a SHA-512 hash. For now, write the pre-computed hash from the default
     * rootfs (forevervigilant). A proper password hashing implementation
     * is future work.
     *
     * TODO: implement SHA-512 crypt in the installer or login binary. */
    {
        int fd = open("/etc/shadow", O_WRONLY | O_CREAT);
        if (fd < 0) { printf("ERROR: cannot write /etc/shadow\n"); return -1; }
        char line[512];
        int n = snprintf(line, sizeof(line),
            "%s:$6$5a3b9c1d2e4f6789$fvwyIjdmyvB59hifGMRFrcwhBb4cH0.3nRy2j2LpCk."
            "aNIFNyvYQJ36Bsl94miFbD/JHICz8O1dXoegZ0OmOg.:19000:0:99999:7:::\n",
            username);
        write(fd, line, (size_t)n);
        close(fd);
    }

    printf("User '%s' configured.\n", username);
    printf("NOTE: Custom password hashing not yet implemented.\n");
    printf("      Default password 'forevervigilant' will be used.\n");
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\n=== Aegis Installer ===\n\n");
    printf("This will install Aegis to your NVMe disk.\n");
    printf("WARNING: All data on the disk will be destroyed!\n\n");

    blkdev_info_t devs[8];
    int ndevs = (int)blkdev_list(devs, sizeof(devs));
    if (ndevs <= 0) {
        printf("ERROR: cannot enumerate block devices\n");
        return 1;
    }

    int target = -1;
    printf("Available disks:\n");
    int i;
    for (i = 0; i < ndevs; i++) {
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

    /* 1. User account setup (writes to ramdisk ext2 before copy) */
    /* NOTE: setup_user writes /etc/passwd and /etc/shadow. If it fails,
     * the default rootfs credentials (root/forevervigilant) are used. */
    if (setup_user() < 0) {
        printf("  Using default credentials (root/forevervigilant)\n");
    }

    /* 2. Write installed grub.cfg to ramdisk ext2 */
    printf("\nWriting grub.cfg... ");
    fflush(stdout);
    if (write_grub_cfg() < 0) return 1;
    printf("done\n");

    /* 3. Strip test binaries from ramdisk ext2 */
    printf("Stripping test binaries... ");
    fflush(stdout);
    strip_test_binaries();
    printf("done\n");

    /* 4. Create GPT */
    printf("Creating GPT partition table... ");
    fflush(stdout);
    if (write_gpt(devname, disk_blocks) < 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("done\n");
    printf("  Partition 1: EFI System (%llu MB)\n", ESP_SECTORS * 512 / (1024*1024));
    printf("  Partition 2: Aegis Root\n");

    /* 5. Rescan partitions */
    printf("Rescanning partitions... ");
    fflush(stdout);
    int nparts = (int)gpt_rescan(devname);
    if (nparts <= 0) {
        printf("FAILED (found %d partitions)\n", nparts);
        return 1;
    }
    printf("done (%d partition(s))\n", nparts);

    /* Find the Aegis root partition */
    ndevs = (int)blkdev_list(devs, sizeof(devs));
    char root_part[16] = "";
    unsigned long long root_blocks = 0;
    for (i = 0; i < ndevs; i++) {
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

    /* 6. Copy rootfs to Aegis root partition */
    printf("Copying root filesystem...\n");
    if (copy_rootfs(root_part, root_blocks) < 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("  done\n");

    /* 7. Install EFI bootloader to ESP */
    printf("Installing EFI bootloader... ");
    fflush(stdout);
    if (install_esp(devname) < 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("done\n");

    /* 8. Sync */
    printf("Syncing disk... ");
    fflush(stdout);
    sync();
    printf("done\n");

    printf("\n=== Installation complete! ===\n");
    printf("Remove the ISO and reboot to start Aegis from disk.\n\n");

    return 0;
}
