# Phase 35: Text-Mode Installer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A text-mode installer that partitions NVMe, copies the live rootfs, installs GRUB, and produces a bootable installed system.

**Architecture:** New block device syscalls (gated by CAP_KIND_DISK_ADMIN) expose raw I/O to userspace. The installer binary writes GPT, copies rootfs from ramdisk to NVMe, writes GRUB boot stages, and configures the installed grub.cfg. Pre-built GRUB images are embedded in rootfs.img at build time.

**Tech Stack:** C (kernel syscalls, userspace installer), Rust (cap kind), Makefile (GRUB build), Python (test)

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/cap/cap.h` | Modify | Add `CAP_KIND_DISK_ADMIN 11u` |
| `kernel/cap/src/lib.rs` | No change | Generic — validates any kind value |
| `kernel/syscall/sys_disk.c` | Create | `sys_blkdev_list`, `sys_blkdev_io`, `sys_gpt_rescan` |
| `kernel/syscall/sys_impl.h` | Modify | Declare new syscalls |
| `kernel/syscall/syscall.c` | Modify | Dispatch cases 510, 511, 512 |
| `kernel/fs/blkdev.c` | Modify | Add `blkdev_count()`, `blkdev_get_index()`, `blkdev_unregister_children()` |
| `kernel/fs/blkdev.h` | Modify | Declare new blkdev functions |
| `kernel/fs/gpt.c` | Modify | Add `gpt_rescan()` that clears old partitions before scanning |
| `kernel/fs/gpt.h` | Modify | Declare `gpt_rescan()` |
| `kernel/syscall/sys_process.c` | Modify | Add DISK_ADMIN to execve baseline |
| `user/installer/main.c` | Create | Installer TUI binary |
| `user/installer/Makefile` | Create | Build rules |
| `Makefile` | Modify | GRUB image build, kernel+GRUB in rootfs, installer in DISK_USER_BINS |
| `tests/test_installer.py` | Create | Two-boot installation test |

---

### Task 1: CAP_KIND_DISK_ADMIN + Block Device Syscalls

Add the new capability kind and three syscalls for block device access.

**Files:**
- Modify: `kernel/cap/cap.h`
- Create: `kernel/syscall/sys_disk.c`
- Modify: `kernel/syscall/sys_impl.h`
- Modify: `kernel/syscall/syscall.c`
- Modify: `kernel/syscall/sys_process.c`
- Modify: `kernel/fs/blkdev.h`
- Modify: `kernel/fs/blkdev.c`
- Modify: `Makefile` — add sys_disk.c to USERSPACE_SRCS

- [ ] **Step 1: Add CAP_KIND_DISK_ADMIN to cap.h**

After `CAP_KIND_PROC_READ 10u`, add:

```c
#define CAP_KIND_DISK_ADMIN 11u  /* may perform raw block device I/O */
```

- [ ] **Step 2: Add blkdev helper functions to blkdev.h and blkdev.c**

In `blkdev.h`, add after `blkdev_get`:

```c
/* Return the number of registered block devices. */
int blkdev_count(void);

/* Return the i-th registered block device (0-based), or NULL if out of range. */
blkdev_t *blkdev_get_index(int i);

/* Unregister all child devices whose name starts with parent_prefix + "p".
 * Used by gpt_rescan to remove old partition blkdevs before re-scanning. */
void blkdev_unregister_children(const char *parent_prefix);
```

In `blkdev.c`, add:

```c
int blkdev_count(void)
{
    return s_count;
}

blkdev_t *blkdev_get_index(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return s_devices[i];
}

void blkdev_unregister_children(const char *parent_prefix)
{
    int plen = 0;
    while (parent_prefix[plen]) plen++;
    int dst = 0;
    int i;
    for (i = 0; i < s_count; i++) {
        /* Check if name starts with prefix + 'p' */
        int match = 1;
        int j;
        for (j = 0; j < plen; j++) {
            if (s_devices[i]->name[j] != parent_prefix[j]) { match = 0; break; }
        }
        if (match && s_devices[i]->name[plen] == 'p') {
            /* Skip this device (unregister) */
            continue;
        }
        s_devices[dst++] = s_devices[i];
    }
    s_count = dst;
}
```

- [ ] **Step 3: Add gpt_rescan to gpt.h and gpt.c**

In `gpt.h`, add:

```c
/* gpt_rescan — unregister old partitions on devname, then scan again.
 * Returns number of new partitions found, or 0 on failure. */
int gpt_rescan(const char *devname);
```

In `gpt.c`, add after `gpt_scan`:

```c
int gpt_rescan(const char *devname)
{
    blkdev_unregister_children(devname);
    return gpt_scan(devname);
}
```

- [ ] **Step 4: Create kernel/syscall/sys_disk.c**

```c
#include "sys_impl.h"
#include "cap.h"
#include "uaccess.h"
#include "printk.h"
#include "blkdev.h"
#include "gpt.h"
#include <stdint.h>

/* ── blkdev_info_t — sent to userspace by sys_blkdev_list ───────────── */
typedef struct {
    char     name[16];
    uint64_t block_count;
    uint32_t block_size;
    uint32_t _pad;
} blkdev_info_t;

/*
 * sys_blkdev_list — syscall 510
 * Enumerate registered block devices.
 * arg1 = user buffer pointer
 * arg2 = buffer size in bytes
 * Returns number of devices, or negative errno.
 */
uint64_t
sys_blkdev_list(uint64_t arg1, uint64_t arg2)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_DISK_ADMIN, CAP_RIGHTS_READ) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    int count = blkdev_count();
    uint64_t max_entries = arg2 / sizeof(blkdev_info_t);
    int i;
    for (i = 0; i < count && (uint64_t)i < max_entries; i++) {
        blkdev_t *d = blkdev_get_index(i);
        if (!d) break;
        blkdev_info_t info;
        __builtin_memset(&info, 0, sizeof(info));
        /* Copy name */
        int j;
        for (j = 0; j < 15 && d->name[j]; j++)
            info.name[j] = d->name[j];
        info.name[j] = '\0';
        info.block_count = d->block_count;
        info.block_size  = d->block_size;
        if (!user_ptr_valid(arg1 + (uint64_t)i * sizeof(blkdev_info_t), sizeof(blkdev_info_t)))
            return (uint64_t)-14;  /* EFAULT */
        copy_to_user((void *)(uintptr_t)(arg1 + (uint64_t)i * sizeof(blkdev_info_t)),
                     &info, sizeof(blkdev_info_t));
    }
    return (uint64_t)i;
}

/*
 * sys_blkdev_io — syscall 511
 * Raw block device read/write.
 * arg1 = user pointer to device name (NUL-terminated)
 * arg2 = LBA start
 * arg3 = block count
 * arg4 = user buffer
 * arg5 = 0=read, 1=write
 * Returns 0 on success, negative errno on failure.
 */
uint64_t
sys_blkdev_io(uint64_t arg1, uint64_t arg2, uint64_t arg3,
              uint64_t arg4, uint64_t arg5)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    uint32_t rights = (arg5 != 0) ? CAP_RIGHTS_WRITE : CAP_RIGHTS_READ;
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_DISK_ADMIN, rights) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    /* Copy device name from user */
    char name[16];
    {
        uint64_t i;
        for (i = 0; i < 15; i++) {
            if (!user_ptr_valid(arg1 + i, 1))
                return (uint64_t)-14;
            char c;
            copy_from_user(&c, (const void *)(uintptr_t)(arg1 + i), 1);
            name[i] = c;
            if (c == '\0') break;
        }
        name[15] = '\0';
    }

    blkdev_t *dev = blkdev_get(name);
    if (!dev) return (uint64_t)-19;  /* ENODEV */

    uint64_t lba   = arg2;
    uint64_t count = arg3;
    if (count == 0) return 0;
    if (lba + count > dev->block_count)
        return (uint64_t)-22;  /* EINVAL */

    /* Transfer in 8-sector (4KB) chunks using a kernel bounce buffer.
     * NVMe driver limits each transfer to count*512 <= 4096. */
    static uint8_t s_bounce[4096];
    uint64_t done = 0;
    while (done < count) {
        uint32_t chunk = (uint32_t)(count - done);
        if (chunk > 8) chunk = 8;
        uint64_t user_off = done * 512;

        if (arg5 == 0) {
            /* Read: dev → bounce → user */
            if (dev->read(dev, lba + done, chunk, s_bounce) < 0)
                return (uint64_t)-5;  /* EIO */
            if (!user_ptr_valid(arg4 + user_off, (uint64_t)chunk * 512))
                return (uint64_t)-14;
            copy_to_user((void *)(uintptr_t)(arg4 + user_off),
                         s_bounce, (uint64_t)chunk * 512);
        } else {
            /* Write: user → bounce → dev */
            if (!user_ptr_valid(arg4 + user_off, (uint64_t)chunk * 512))
                return (uint64_t)-14;
            copy_from_user(s_bounce,
                           (const void *)(uintptr_t)(arg4 + user_off),
                           (uint64_t)chunk * 512);
            if (dev->write(dev, lba + done, chunk, s_bounce) < 0)
                return (uint64_t)-5;  /* EIO */
        }
        done += chunk;
    }
    return 0;
}

/*
 * sys_gpt_rescan — syscall 512
 * Re-scan GPT on a named block device.
 * arg1 = user pointer to device name
 * Returns number of partitions, or negative errno.
 */
uint64_t
sys_gpt_rescan(uint64_t arg1)
{
    aegis_process_t *proc = (aegis_process_t *)sched_current();
    if (cap_check(proc->caps, CAP_TABLE_SIZE,
                  CAP_KIND_DISK_ADMIN, CAP_RIGHTS_WRITE) != 0)
        return (uint64_t)-(int64_t)ENOCAP;

    char name[16];
    {
        uint64_t i;
        for (i = 0; i < 15; i++) {
            if (!user_ptr_valid(arg1 + i, 1))
                return (uint64_t)-14;
            char c;
            copy_from_user(&c, (const void *)(uintptr_t)(arg1 + i), 1);
            name[i] = c;
            if (c == '\0') break;
        }
        name[15] = '\0';
    }

    int n = gpt_rescan(name);
    return (uint64_t)(int64_t)n;
}
```

- [ ] **Step 5: Add declarations to sys_impl.h**

```c
uint64_t sys_blkdev_list(uint64_t arg1, uint64_t arg2);
uint64_t sys_blkdev_io(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5);
uint64_t sys_gpt_rescan(uint64_t arg1);
```

- [ ] **Step 6: Add dispatch cases to syscall.c**

After the last case in the `switch (num)` block, add:

```c
    case 510: return sys_blkdev_list(arg1, arg2);
    case 511: return sys_blkdev_io(arg1, arg2, arg3, arg4, arg5);
    case 512: return sys_gpt_rescan(arg1);
```

- [ ] **Step 7: Add DISK_ADMIN to execve baseline in sys_process.c**

After the `cap_grant(... CAP_KIND_PROC_READ ...)` line in the execve function, add:

```c
        cap_grant(proc->caps, CAP_TABLE_SIZE, CAP_KIND_DISK_ADMIN, CAP_RIGHTS_READ | CAP_RIGHTS_WRITE);
```

- [ ] **Step 8: Add sys_disk.c to Makefile USERSPACE_SRCS**

```makefile
USERSPACE_SRCS = \
    kernel/syscall/syscall.c \
    kernel/syscall/sys_io.c \
    kernel/syscall/sys_memory.c \
    kernel/syscall/sys_process.c \
    kernel/syscall/sys_file.c \
    kernel/syscall/sys_signal.c \
    kernel/syscall/sys_socket.c \
    kernel/syscall/sys_random.c \
    kernel/syscall/sys_disk.c \
    kernel/syscall/futex.c \
    kernel/proc/proc.c \
    kernel/elf/elf.c \
```

- [ ] **Step 9: Build and verify**

```bash
make INIT=vigil iso 2>&1 | grep -i error
```

Expected: clean compile, zero warnings.

- [ ] **Step 10: Commit**

```bash
git add kernel/cap/cap.h kernel/syscall/sys_disk.c kernel/syscall/sys_impl.h \
        kernel/syscall/syscall.c kernel/syscall/sys_process.c \
        kernel/fs/blkdev.h kernel/fs/blkdev.c kernel/fs/gpt.h kernel/fs/gpt.c \
        Makefile
git commit -m "feat: add block device syscalls and CAP_KIND_DISK_ADMIN

sys_blkdev_list (510): enumerate block devices
sys_blkdev_io (511): raw read/write via bounce buffer
sys_gpt_rescan (512): re-scan GPT after partition table write
CAP_KIND_DISK_ADMIN (11): gates all three syscalls
Added to execve baseline for now (security audit Phase 38)."
```

---

### Task 2: GRUB Build Infrastructure + Kernel in rootfs

Build GRUB boot images at `make` time and embed them (plus aegis.elf) in rootfs.img.

**Files:**
- Modify: `Makefile`

- [ ] **Step 1: Add GRUB image build targets**

After the `build-musl` target, add:

```makefile
# ── GRUB boot images for installer ─────────────────────────────────────────
# boot.img: MBR bootstrap (440 bytes) — loads core.img from BIOS boot partition
# core.img: main GRUB binary with ext2 + multiboot2 support
GRUB_BOOT = $(BUILD)/grub-boot.img
GRUB_CORE = $(BUILD)/grub-core.img

$(GRUB_BOOT):
	@mkdir -p $(BUILD)
	cp /usr/lib/grub/i386-pc/boot.img $(GRUB_BOOT)

$(GRUB_CORE):
	@mkdir -p $(BUILD)
	grub-mkimage -O i386-pc -o $(GRUB_CORE) \
	    -p '(hd0,gpt2)/boot/grub' \
	    biosdisk part_gpt ext2 normal multiboot2 boot configfile
```

- [ ] **Step 2: Add kernel + GRUB to rootfs.img**

In the `$(ROOTFS)` target, after the curl/CA bundle debugfs commands and BEFORE the final `@echo "Root filesystem image created"` line, add:

```makefile
	# Kernel binary + GRUB boot images for installer
	printf 'mkdir /boot\nmkdir /boot/grub\n' \
	    | /sbin/debugfs -w $(ROOTFS)
	printf 'write $(BUILD)/aegis.elf /boot/aegis.elf\nwrite $(GRUB_BOOT) /boot/grub/boot.img\nwrite $(GRUB_CORE) /boot/grub/core.img\n' \
	    | /sbin/debugfs -w $(ROOTFS)
```

- [ ] **Step 3: Add GRUB images as rootfs prerequisites**

Change the `$(ROOTFS)` dependency line to include the GRUB images and the kernel ELF:

```makefile
$(ROOTFS): $(DISK_USER_BINS) $(BUILD)/aegis.elf $(GRUB_BOOT) $(GRUB_CORE)
```

Note: `$(BUILD)/aegis.elf` is needed because we write it into rootfs. This creates a dependency cycle concern — the ELF depends on blob objects which depend on user ELFs, and rootfs depends on the ELF. But rootfs is only used by the ISO target, and the ELF is built independently. The Make dependency DAG is: user ELFs → blobs → aegis.elf → rootfs → ISO. This is acyclic.

- [ ] **Step 4: Build and verify**

```bash
make rootfs 2>&1 | tail -5
echo 'ls /boot/grub' | /sbin/debugfs build/rootfs.img 2>&1
```

Expected: `boot.img` and `core.img` visible in `/boot/grub/`.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "feat(build): add GRUB boot images and kernel to rootfs.img

Pre-build boot.img (MBR bootstrap) and core.img (GRUB binary
with ext2+multiboot2+part_gpt) for the installer. Copy kernel
ELF to /boot/aegis.elf in rootfs for installed-system boot."
```

---

### Task 3: Installer Binary

Create the userspace installer program.

**Files:**
- Create: `user/installer/main.c`
- Create: `user/installer/Makefile`
- Modify: `Makefile` — add installer to DISK_USER_BINS and build rules

- [ ] **Step 1: Create user/installer/Makefile**

```makefile
MUSL_DIR = ../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none

all: installer.elf

installer.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.elf
```

- [ ] **Step 2: Create user/installer/main.c**

This is a ~400-line C program. Key sections:

1. **Syscall wrappers** for sys_blkdev_list (510), sys_blkdev_io (511), sys_gpt_rescan (512)
2. **CRC32** implementation (same polynomial as kernel gpt.c)
3. **GPT writer** — creates protective MBR + GPT header + 2 partition entries
4. **Rootfs copier** — reads from ramdisk0, writes to NVMe partition
5. **GRUB installer** — reads boot.img/core.img from ext2, writes to MBR+BIOS boot partition
6. **grub.cfg writer** — writes installed grub.cfg to ext2 /boot/grub/grub.cfg
7. **Interactive TUI** — prompts user, shows progress

The full source is provided in the step below.

- [ ] **Step 3: Write the installer main.c**

```c
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
```

- [ ] **Step 4: Add installer to Makefile**

Add build rule after the other user binary rules:

```makefile
user/installer/installer.elf: user/installer/main.c $(MUSL_BUILT)
	$(MAKE) -C user/installer
```

Add to `DISK_USER_BINS`:
```makefile
	user/installer/installer.elf \
```

Add the debugfs write to rootfs population (in the large printf block that writes user binaries):
```
write user/installer/installer.elf /bin/installer\n
```

- [ ] **Step 5: Build and verify**

```bash
make rootfs 2>&1 | tail -5
echo 'stat /bin/installer' | /sbin/debugfs build/rootfs.img 2>&1
```

- [ ] **Step 6: Commit**

```bash
git add user/installer/main.c user/installer/Makefile Makefile
git commit -m "feat: add text-mode installer binary

Userspace installer: partitions NVMe with Aegis GPT GUIDs,
copies rootfs from ramdisk, installs GRUB boot stages, writes
installed grub.cfg targeting (hd0,gpt2)."
```

---

### Task 4: Installation Test

Create a two-boot test that installs to a fresh NVMe disk, then boots from it.

**Files:**
- Create: `tests/test_installer.py`
- Modify: `tests/run_tests.sh`

- [ ] **Step 1: Create tests/test_installer.py**

```python
#!/usr/bin/env python3
"""test_installer.py — Two-boot installation test.

Boot 1: Live ISO + empty NVMe → run installer → shutdown
Boot 2: NVMe only (no ISO) → verify boot + login
"""
import subprocess, sys, os, select, fcntl, time, socket, tempfile

QEMU = "qemu-system-x86_64"
ISO  = "build/aegis.iso"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_TIMEOUT = 120
CMD_TIMEOUT  = 60

_KEY_MAP = {
    ' ':  'spc', '\n': 'ret', '/':  'slash', '-':  'minus', '.':  'dot',
    ':':  'shift-semicolon', '|':  'shift-backslash', '_':  'shift-minus',
    '<':  'shift-comma', '>':  'shift-dot', '&':  'shift-7',
    '=': 'equal', '(': 'shift-9', ')': 'shift-0', '"': 'shift-apostrophe',
    '!': 'shift-1', '?': 'shift-slash', ',': 'comma',
}
for c in 'abcdefghijklmnopqrstuvwxyz':
    _KEY_MAP[c] = c
for c in '0123456789':
    _KEY_MAP.setdefault(c, c)

def _type_string(mon_sock, s):
    for ch in s:
        key = _KEY_MAP.get(ch)
        if key is None: continue
        mon_sock.sendall(f'sendkey {key}\n'.encode())
        time.sleep(0.08)
        try: mon_sock.recv(4096)
        except OSError: pass

def _drain(fd, timeout=0.5):
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                chunk = os.read(fd, 65536)
                if chunk: buf += chunk
            except (BlockingIOError, OSError): pass
    return buf

def _wait_for(fd, needle, timeout):
    enc = needle.encode() if isinstance(needle, str) else needle
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.5)
        if ready:
            try:
                chunk = os.read(fd, 65536)
                if chunk: buf += chunk
            except (BlockingIOError, OSError): pass
        if enc in buf:
            return True, buf
    return False, buf

def run_test():
    iso_path = os.path.join(ROOT, ISO)
    if not os.path.exists(iso_path):
        print("SKIP: ISO not found")
        sys.exit(0)

    # Create empty 128MB NVMe disk
    fd_disk, disk_path = tempfile.mkstemp(suffix=".img")
    os.close(fd_disk)
    subprocess.run(["dd", "if=/dev/zero", f"of={disk_path}", "bs=1M", "count=128"],
                   capture_output=True)

    mon_path = tempfile.mktemp(suffix=".sock")

    try:
        # ── Boot 1: Live ISO + empty NVMe ──
        print("Boot 1: Live ISO + installer...")
        proc = subprocess.Popen([
            QEMU, "-machine", "q35", "-cpu", "Broadwell",
            "-cdrom", iso_path, "-boot", "order=d",
            "-display", "none", "-vga", "std", "-nodefaults",
            "-serial", "stdio", "-no-reboot", "-m", "2G",
            "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
            "-device", "nvme,drive=nvme0,serial=aegis0",
            "-device", "virtio-net-pci,netdev=n0,disable-legacy=on",
            "-netdev", "user,id=n0",
            "-monitor", f"unix:{mon_path},server,nowait"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        flags = fcntl.fcntl(proc.stdout.fileno(), fcntl.F_GETFL)
        fcntl.fcntl(proc.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

        # Wait for monitor socket
        deadline = time.time() + 10
        while not os.path.exists(mon_path) and time.time() < deadline:
            time.sleep(0.1)
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon.connect(mon_path)
        mon.setblocking(False)

        # Login
        ok, _ = _wait_for(proc.stdout.fileno(), "login: ", BOOT_TIMEOUT)
        if not ok:
            print("FAIL: login prompt not found in boot 1")
            sys.exit(1)
        _type_string(mon, "root\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "assword", 10)
        if not ok:
            print("FAIL: password prompt not found in boot 1")
            sys.exit(1)
        _type_string(mon, "forevervigilant\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "# ", 10)
        if not ok:
            print("FAIL: shell prompt not found in boot 1")
            sys.exit(1)

        # Run installer, answer y
        time.sleep(1)
        _type_string(mon, "installer\n")
        ok, _ = _wait_for(proc.stdout.fileno(), "[y/N]", CMD_TIMEOUT)
        if not ok:
            print("FAIL: installer prompt not found")
            sys.exit(1)
        _type_string(mon, "y\n")

        # Wait for installation to complete
        ok, buf = _wait_for(proc.stdout.fileno(), "Installation complete", CMD_TIMEOUT)
        if not ok:
            print("FAIL: installation did not complete")
            print("  tail:", buf[-500:].decode("utf-8", errors="replace"))
            sys.exit(1)
        print("  Installation complete in boot 1")

        # Shutdown
        time.sleep(1)
        _type_string(mon, "exit\n")
        time.sleep(3)
        try: mon.close()
        except OSError: pass
        proc.kill()
        proc.wait()
        try: os.unlink(mon_path)
        except OSError: pass

        # ── Boot 2: NVMe only (no ISO) ──
        print("Boot 2: NVMe only (installed system)...")
        mon_path2 = tempfile.mktemp(suffix=".sock")
        proc2 = subprocess.Popen([
            QEMU, "-machine", "q35", "-cpu", "Broadwell",
            "-display", "none", "-vga", "std", "-nodefaults",
            "-serial", "stdio", "-no-reboot", "-m", "2G",
            "-drive", f"file={disk_path},if=none,id=nvme0,format=raw",
            "-device", "nvme,drive=nvme0,serial=aegis0",
            "-monitor", f"unix:{mon_path2},server,nowait"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        flags = fcntl.fcntl(proc2.stdout.fileno(), fcntl.F_GETFL)
        fcntl.fcntl(proc2.stdout.fileno(), fcntl.F_SETFL, flags | os.O_NONBLOCK)

        deadline = time.time() + 10
        while not os.path.exists(mon_path2) and time.time() < deadline:
            time.sleep(0.1)
        mon2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon2.connect(mon_path2)
        mon2.setblocking(False)

        # Wait for login prompt from installed system
        ok, buf = _wait_for(proc2.stdout.fileno(), "login: ", BOOT_TIMEOUT)
        if not ok:
            print("FAIL: login prompt not found in boot 2 (installed system)")
            print("  tail:", buf[-500:].decode("utf-8", errors="replace"))
            sys.exit(1)
        print("  Login prompt found on installed system")

        # Verify no [RAMDISK] in output (installed system should not use ramdisk)
        if b"[RAMDISK]" in buf:
            print("FAIL: installed system should not have [RAMDISK]")
            sys.exit(1)
        print("  No [RAMDISK] in boot output (correct)")

        # Verify [EXT2] mounted nvme0p1
        if b"[EXT2] OK: mounted nvme0p1" not in buf:
            print("FAIL: installed system should mount nvme0p1")
            print("  tail:", buf[-500:].decode("utf-8", errors="replace"))
            sys.exit(1)
        print("  [EXT2] OK: mounted nvme0p1 (correct)")

        try: mon2.close()
        except OSError: pass
        proc2.kill()
        proc2.wait()
        try: os.unlink(mon_path2)
        except OSError: pass

        print("PASS test_installer")

    finally:
        try: os.unlink(disk_path)
        except OSError: pass

if __name__ == "__main__":
    run_test()
```

- [ ] **Step 2: Add test_installer to run_tests.sh**

After the `test_curl` line:

```bash
echo "--- test_installer ---"
python3 tests/test_installer.py
```

- [ ] **Step 3: Run test**

```bash
python3 tests/test_installer.py
```

Expected: Boot 1 completes installation, Boot 2 boots from NVMe with login prompt.

- [ ] **Step 4: Commit**

```bash
git add tests/test_installer.py tests/run_tests.sh
git commit -m "test: add two-boot installer test

Boot 1: live ISO + empty NVMe, run installer, verify completion.
Boot 2: NVMe only, verify login prompt, no RAMDISK, ext2 on nvme0p1."
```

---

### Task 5: Update CLAUDE.md

**Files:**
- Modify: `.claude/CLAUDE.md`

- [ ] **Step 1: Update Build Status table**

Add after the Writable root row:

```
| Installer (Phase 35) | ✅ | Text-mode installer; GPT+GRUB+rootfs copy; sys_blkdev_io/list/gpt_rescan; test_installer PASS |
```

- [ ] **Step 2: Update Phase Roadmap**

Change Phase 35 from `Not started` to `✅ Done`.

- [ ] **Step 3: Add Phase 35 Forward Constraints**

```markdown
## Phase 35 — Forward Constraints

1. **No UEFI boot.** BIOS+GPT only. UEFI requires EFI System Partition + different GRUB image.
2. **Single NVMe only.** Installer targets `nvme0`. No device selection menu.
3. **No resize/dual-boot.** Installer wipes entire disk.
4. **GRUB prefix hardcoded.** `(hd0,gpt2)/boot/grub`. If partition layout changes, rebuild core.img.
5. **DISK_ADMIN in execve baseline.** Every process gets it. Phase 38 security audit should restrict.
6. **No installed-system kernel update path.** Must re-run installer to update kernel.
7. **grub.cfg uses (hd0,gpt2) not UUID.** Works for single-disk. Multi-disk needs search --fs-uuid.
```

- [ ] **Step 4: Commit**

```bash
git add .claude/CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 35 installer"
```

---

## Self-Review

**Spec coverage:**
- Component 1 (Block device syscalls) → Task 1 ✅
- Component 2 (CAP_KIND_DISK_ADMIN) → Task 1 step 1 ✅
- Component 3 (GPT creation) → Task 3 (installer main.c write_gpt) ✅
- Component 4 (Rootfs copy) → Task 3 (installer main.c copy_rootfs) ✅
- Component 5 (GRUB installation) → Task 2 (build infra) + Task 3 (installer write) ✅
- Component 6 (Kernel in rootfs) → Task 2 step 2 ✅
- Component 7 (Installer binary) → Task 3 ✅
- Component 8 (Installed boot path) → Verified by Task 4 test ✅
- Testing → Task 4 ✅

**Placeholder scan:** No TBD/TODO. All code is complete.

**Type consistency:** `blkdev_info_t` matches between sys_disk.c and installer main.c. Syscall numbers 510/511/512 consistent. `CAP_KIND_DISK_ADMIN = 11u` consistent between cap.h usage and sys_disk.c check.

**Key ordering note for Task 3:** The installer writes grub.cfg to the ramdisk ext2 BEFORE copying to NVMe. This way the copy includes grub.cfg. The order in main() is: write_grub_cfg → write_gpt → gpt_rescan → copy_rootfs → install_grub → sync.
