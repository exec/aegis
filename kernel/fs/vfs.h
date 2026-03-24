#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define PROC_MAX_FDS 16 /* was 8; bumped for Phase 16 pipe fd budget */

/* ── stat types and mode constants ──────────────────────────────────────── */

/* S_IF* constants — match Linux/POSIX values used in st_mode */
#define S_IFMT   0170000U
#define S_IFREG  0100000U  /* regular file */
#define S_IFDIR  0040000U  /* directory */
#define S_IFCHR  0020000U  /* character device */
#define S_IFIFO  0010000U  /* FIFO / pipe */

/* makedev — encode major/minor into rdev field (Linux encoding) */
#define makedev(maj, min) \
    (((uint64_t)(maj) << 8) | (uint64_t)((min) & 0xFF))

/*
 * k_stat_t — kernel-side stat structure. Layout MUST match Linux x86-64
 * struct stat exactly (musl passes a pointer to struct stat directly).
 * Verified against musl arch/x86_64/bits/stat.h: total = 144 bytes.
 */
typedef struct {
    uint64_t st_dev;        /*   0 */
    uint64_t st_ino;        /*   8 */
    uint64_t st_nlink;      /*  16 */
    uint32_t st_mode;       /*  24 */
    uint32_t st_uid;        /*  28 */
    uint32_t st_gid;        /*  32 */
    uint32_t __pad0;        /*  36 */
    uint64_t st_rdev;       /*  40 */
    int64_t  st_size;       /*  48 */
    int64_t  st_blksize;    /*  56 */
    int64_t  st_blocks;     /*  64 */
    int64_t  st_atime;      /*  72 */
    int64_t  st_atime_nsec; /*  80 */
    int64_t  st_mtime;      /*  88 */
    int64_t  st_mtime_nsec; /*  96 */
    int64_t  st_ctime;      /* 104 */
    int64_t  st_ctime_nsec; /* 112 */
    int64_t  __unused[3];   /* 120 */
} k_stat_t;

_Static_assert(sizeof(k_stat_t) == 144,
    "k_stat_t must be 144 bytes (Linux x86-64 struct stat)");

/* ── VFS operations vtable ───────────────────────────────────────────────── */

/* File operations vtable. Each open file carries a pointer to its driver's ops. */
typedef struct {
    /* read — copy up to len bytes starting at off into buf (kernel buffer).
     * Returns bytes copied (0 = EOF, negative = error). */
    int (*read)(void *priv, void *buf, uint64_t off, uint64_t len);
    /* write — copy len bytes from user-space buf to device.
     * Returns bytes written, or negative errno. NULL = not writable. */
    int (*write)(void *priv, const void *buf, uint64_t len);
    /* close — release any driver-side resources for this file. */
    void (*close)(void *priv);
    /* readdir -- fill name_out (>=256 bytes) and type_out with the entry at index.
     * Returns 0 on success, -1 if index is past the last entry.
     * type: DT_REG=8, DT_DIR=4.
     * Set to NULL for non-directory fds (e.g. console, kbd). */
    int (*readdir)(void *priv, uint64_t index, char *name_out, uint8_t *type_out);
    /* dup — called when this fd is duplicated (dup/dup2/fork).
     * Increment any reference counts held by this driver.
     * NULL = stateless driver, no action needed (initrd, console, kbd). */
    void (*dup)(void *priv);
    /* stat — fill *st with file metadata.
     * NULL = driver has no stat; sys_fstat synthesizes a minimal stat.
     * Returns 0 on success. */
    int (*stat)(void *priv, k_stat_t *st);
} vfs_ops_t;

/* Open file descriptor. Embedded inline in aegis_process_t.fds[].
 * ops == NULL means the slot is free. */
typedef struct {
    const vfs_ops_t *ops;    /* NULL = free slot */
    void            *priv;   /* driver-private data */
    uint64_t         offset; /* current read position */
    uint64_t         size;   /* file size in bytes; 0 for devices/directories */
    uint32_t         flags;  /* open flags: O_RDONLY(0)/O_WRONLY(1)/O_RDWR(2)/O_NONBLOCK */
    uint32_t         _pad;   /* padding; keeps struct 8-byte aligned (total = 40 bytes) */
} vfs_file_t;

_Static_assert(sizeof(vfs_file_t) == 40, "vfs_file_t must be 40 bytes");

/* vfs_init — print [VFS] OK line and register built-in drivers.
 * Called from kernel_main before sched_init. */
void vfs_init(void);

/* Linux O_CREAT flag value (used by vfs_open for ext2 file creation) */
#define VFS_O_CREAT 0x40U

/* VFS_O_CLOEXEC — Linux O_CLOEXEC flag value, as passed in open/pipe2 flags arg.
 * Must match VFS_FD_CLOEXEC so that `flags & VFS_O_CLOEXEC` maps to the fd bit. */
#define VFS_O_CLOEXEC 0x80000U

/* FD_CLOEXEC — close-on-exec flag stored in vfs_file_t.flags.
 * Set when O_CLOEXEC is passed to open/pipe2; cleared on dup/dup2 (POSIX).
 * Same bit value as VFS_O_CLOEXEC by design — no shift needed at install time. */
#define VFS_FD_CLOEXEC 0x80000U

_Static_assert(VFS_O_CLOEXEC == VFS_FD_CLOEXEC,
    "VFS_O_CLOEXEC and VFS_FD_CLOEXEC must be the same bit value");

/* vfs_open — find a file by path across all registered drivers.
 * flags: open flags (e.g. VFS_O_CREAT to create the file if missing on ext2).
 * Populates *out on success; returns 0 on success, -2 (ENOENT) if not found.
 * Called by sys_open to resolve path to a vfs_file_t. */
int vfs_open(const char *path, int flags, vfs_file_t *out);

/* vfs_stat_path — stat a file by path.
 * Handles initrd files, directory paths (/,/etc,/bin), and /dev/ specials.
 * Returns 0 on success, -2 (ENOENT) if not found. */
int vfs_stat_path(const char *path, k_stat_t *out);

#endif /* VFS_H */
