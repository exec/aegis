/* gpt.c — GPT partition table writing + CRC32 (libinstall) */
#include "libinstall.h"
#include "syscalls.h"
#include <string.h>

/* ── CRC32 (file-local) ─────────────────────────────────────────────── */

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

static unsigned int crc32_calc(const void *data, unsigned int len)
{
    if (!crc32_ready) crc32_init();
    const unsigned char *p = data;
    unsigned int crc = 0xFFFFFFFF;
    unsigned int i;
    for (i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ── Protective MBR ─────────────────────────────────────────────────── */

static void write_protective_mbr(unsigned char *mbr,
                                  unsigned long long disk_sectors)
{
    memset(mbr, 0, 512);
    mbr[446] = 0x00;
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00;
    mbr[450] = 0xEE;
    mbr[451] = 0xFF; mbr[452] = 0xFF; mbr[453] = 0xFF;
    mbr[454] = 0x01; mbr[455] = 0; mbr[456] = 0; mbr[457] = 0;
    unsigned int sz = (disk_sectors - 1 > 0xFFFFFFFFULL)
                      ? 0xFFFFFFFFu : (unsigned int)(disk_sectors - 1);
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

/* ESP layout */
#define ESP_START   2048ULL
#define ESP_SECTORS 65536ULL
#define ESP_END     (ESP_START + ESP_SECTORS - 1)
#define ROOT_START  (ESP_END + 1)

/* ── Error reporting helper ─────────────────────────────────────────── */

static void report_err(install_progress_t *p, const char *msg)
{
    if (p && p->on_error)
        p->on_error(msg, p->ctx);
}

/* ── Public: install_write_gpt ──────────────────────────────────────── */

int install_write_gpt(const char *devname, uint64_t disk_blocks,
                      install_progress_t *p)
{
    static unsigned char sector[512];
    static unsigned char entries[128 * 128];
    unsigned long long last_lba = disk_blocks - 1;
    unsigned long long root_end = last_lba - 33;

    if (p && p->on_step)
        p->on_step("Writing partition table", p->ctx);

    if (root_end <= ROOT_START) {
        report_err(p, "disk too small");
        return -1;
    }

    write_protective_mbr(sector, disk_blocks);
    if (li_blkdev_io(devname, 0, 1, sector, 1) < 0) {
        report_err(p, "write protective MBR failed");
        return -1;
    }

    memset(entries, 0, sizeof(entries));

    /* Entry 0: EFI System Partition */
    memcpy(&entries[0], ESP_GUID, 16);
    entries[16] = 0x01; entries[17] = 0x02;
    entries[18] = 0x03; entries[19] = 0x04;
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

    unsigned int entry_crc = crc32_calc(entries, 128 * 128);

    /* Primary GPT header (LBA 1) */
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    unsigned int rev = 0x00010000;
    memcpy(&sector[8], &rev, 4);
    unsigned int hsz = 92;
    memcpy(&sector[12], &hsz, 4);
    {
        unsigned long long v;
        v = 1;                 memcpy(&sector[24], &v, 8); /* my_lba */
        v = last_lba;          memcpy(&sector[32], &v, 8); /* alt_lba */
        v = 34;                memcpy(&sector[40], &v, 8); /* first_usable */
        v = last_lba - 33;     memcpy(&sector[48], &v, 8); /* last_usable */
    }
    sector[56] = 0xAE; sector[57] = 0x61;
    sector[58] = 0x15; sector[59] = 0x00;
    {
        unsigned long long v = 2;
        memcpy(&sector[72], &v, 8); /* partition_entry_lba */
    }
    unsigned int nentries = 128;
    memcpy(&sector[80], &nentries, 4);
    unsigned int entry_sz = 128;
    memcpy(&sector[84], &entry_sz, 4);
    memcpy(&sector[88], &entry_crc, 4);
    unsigned int hcrc = crc32_calc(sector, 92);
    memcpy(&sector[16], &hcrc, 4);
    if (li_blkdev_io(devname, 1, 1, sector, 1) < 0) {
        report_err(p, "write primary GPT header failed");
        return -1;
    }

    /* Write partition entries (LBAs 2-33) */
    unsigned long long lba;
    for (lba = 0; lba < 32; lba++) {
        if (li_blkdev_io(devname, 2 + lba, 1,
                         entries + lba * 512, 1) < 0) {
            report_err(p, "write partition entries failed");
            return -1;
        }
    }

    /* Backup entries */
    for (lba = 0; lba < 32; lba++) {
        if (li_blkdev_io(devname, last_lba - 32 + lba, 1,
                         entries + lba * 512, 1) < 0) {
            report_err(p, "write backup entries failed");
            return -1;
        }
    }

    /* Backup GPT header (last LBA) */
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    memcpy(&sector[8], &rev, 4);
    memcpy(&sector[12], &hsz, 4);
    {
        unsigned long long v;
        v = last_lba;          memcpy(&sector[24], &v, 8);
        v = 1;                 memcpy(&sector[32], &v, 8);
        v = 34;                memcpy(&sector[40], &v, 8);
        v = last_lba - 33;     memcpy(&sector[48], &v, 8);
    }
    sector[56] = 0xAE; sector[57] = 0x61;
    sector[58] = 0x15; sector[59] = 0x00;
    {
        unsigned long long v = last_lba - 32;
        memcpy(&sector[72], &v, 8);
    }
    memcpy(&sector[80], &nentries, 4);
    memcpy(&sector[84], &entry_sz, 4);
    memcpy(&sector[88], &entry_crc, 4);
    memset(&sector[16], 0, 4);
    hcrc = crc32_calc(sector, 92);
    memcpy(&sector[16], &hcrc, 4);
    if (li_blkdev_io(devname, last_lba, 1, sector, 1) < 0) {
        report_err(p, "write backup GPT header failed");
        return -1;
    }

    if (p && p->on_progress)
        p->on_progress(100, p->ctx);
    return 0;
}

/* ── Public: install_rescan_gpt ─────────────────────────────────────── */

int install_rescan_gpt(const char *devname)
{
    return (int)li_gpt_rescan(devname);
}
