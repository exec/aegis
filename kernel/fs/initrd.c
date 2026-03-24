#include "initrd.h"
#include "printk.h"
#include <stdint.h>

#ifndef EISDIR
#define EISDIR 21
#endif

/* /etc/motd content — starts with '[' so it survives the make test ANSI filter.
 * The filter keeps only lines starting with '['; content not matching is
 * silently dropped from the serial diff. */
static const char s_motd[] = "[MOTD] Welcome to Aegis\n";

/* Binary ELF blobs embedded by the Makefile via objcopy.
 * These symbols are resolved at link time; their lengths are not
 * compile-time constants, so they cannot appear in static initializers.
 * We store pointers to the length variables and dereference them at
 * open time instead. */
extern const unsigned char shell_elf[];
extern const unsigned int  shell_elf_len;
extern const unsigned char ls_elf[];
extern const unsigned int  ls_elf_len;
extern const unsigned char cat_elf[];
extern const unsigned int  cat_elf_len;
extern const unsigned char echo_elf[];
extern const unsigned int  echo_elf_len;
extern const unsigned char pwd_elf[];
extern const unsigned int  pwd_elf_len;
extern const unsigned char uname_elf[];
extern const unsigned int  uname_elf_len;
extern const unsigned char clear_elf[];
extern const unsigned int  clear_elf_len;
extern const unsigned char true_bin_elf[];
extern const unsigned int  true_bin_elf_len;
extern const unsigned char false_bin_elf[];
extern const unsigned int  false_bin_elf_len;
extern const unsigned char wc_elf[];
extern const unsigned int  wc_elf_len;
extern const unsigned char grep_elf[];
extern const unsigned int  grep_elf_len;
extern const unsigned char sort_elf[];
extern const unsigned int  sort_elf_len;
extern const unsigned char mkdir_elf[];
extern const unsigned int  mkdir_elf_len;
extern const unsigned char touch_elf[];
extern const unsigned int  touch_elf_len;
extern const unsigned char rm_elf[];
extern const unsigned int  rm_elf_len;
extern const unsigned char cp_elf[];
extern const unsigned int  cp_elf_len;
extern const unsigned char mv_elf[];
extern const unsigned int  mv_elf_len;

/* initrd_entry_t — each entry holds a path, a pointer to file data, and a
 * pointer to the file's size variable (link-time value from objcopy/bin2c).
 * Using a pointer-to-size avoids the "initializer element is not constant"
 * error that arises when placing extern uint variables in static initializers. */
typedef struct {
    const char         *name;
    const char         *data;
    const unsigned int *size_ptr;  /* points to xxx_elf_len symbol */
} initrd_entry_t;

static const initrd_entry_t s_files[] = {
    { "/etc/motd",  s_motd,                (const unsigned int *)0 },  /* size set via sizeof */
    { "/bin/sh",    (const char *)shell_elf,     &shell_elf_len     },
    { "/bin/ls",    (const char *)ls_elf,        &ls_elf_len        },
    { "/bin/cat",   (const char *)cat_elf,       &cat_elf_len       },
    { "/bin/echo",  (const char *)echo_elf,      &echo_elf_len      },
    { "/bin/pwd",   (const char *)pwd_elf,       &pwd_elf_len       },
    { "/bin/uname", (const char *)uname_elf,     &uname_elf_len     },
    { "/bin/clear", (const char *)clear_elf,     &clear_elf_len     },
    { "/bin/true",  (const char *)true_bin_elf,  &true_bin_elf_len  },
    { "/bin/false", (const char *)false_bin_elf, &false_bin_elf_len },
    { "/bin/wc",    (const char *)wc_elf,        &wc_elf_len        },
    { "/bin/grep",  (const char *)grep_elf,      &grep_elf_len      },
    { "/bin/sort",  (const char *)sort_elf,      &sort_elf_len      },
    { "/bin/mkdir", (const char *)mkdir_elf,     &mkdir_elf_len     },
    { "/bin/touch", (const char *)touch_elf,     &touch_elf_len     },
    { "/bin/rm",    (const char *)rm_elf,        &rm_elf_len        },
    { "/bin/cp",    (const char *)cp_elf,        &cp_elf_len        },
    { "/bin/mv",    (const char *)mv_elf,        &mv_elf_len        },
    { (const char *)0, (const char *)0, (const unsigned int *)0 }  /* sentinel */
};

/* s_motd_size is a compile-time constant separate from the size_ptr scheme. */
static const uint32_t s_motd_size = sizeof(s_motd) - 1;

static const uint32_t s_nfiles = 18;

/* Helper: return file size for an entry. */
static uint32_t
entry_size(const initrd_entry_t *e)
{
    if (e->size_ptr == (const unsigned int *)0)
        return s_motd_size;   /* /etc/motd — only entry with NULL size_ptr */
    return (uint32_t)*e->size_ptr;
}

/* ── Regular file ops ──────────────────────────────────────────────────── */

static int
initrd_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    const initrd_entry_t *e = (const initrd_entry_t *)priv;
    uint32_t sz = entry_size(e);
    if (off >= sz) return 0;
    uint64_t avail = sz - off;
    if (len > avail) len = avail;
    __builtin_memcpy(buf, e->data + off, len);
    return (int)len;
}

static void
initrd_close_fn(void *priv)
{
    (void)priv; /* static data, nothing to free */
}

static int
initrd_stat_fn(void *priv, k_stat_t *st)
{
    const initrd_entry_t *e = (const initrd_entry_t *)priv;
    uint32_t sz = entry_size(e);

    /* Compute index of this entry in s_files for a synthetic inode */
    uint64_t ino = 1;
    {
        uint32_t i;
        for (i = 0; s_files[i].name != (const char *)0; i++) {
            if (&s_files[i] == e) { ino = (uint64_t)(i + 2); break; }
        }
    }

    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev     = 1;
    st->st_ino     = ino;
    st->st_nlink   = 1;
    st->st_mode    = S_IFREG | 0444;
    st->st_size    = (int64_t)sz;
    st->st_blksize = 512;
    st->st_blocks  = (int64_t)(((uint64_t)sz + 511) / 512 * 8);
    return 0;
}

static int
dir_stat_fn(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 1;
    st->st_ino   = 1;  /* root dir inode */
    st->st_nlink = 2;
    st->st_mode  = S_IFDIR | 0555;
    return 0;
}

static const vfs_ops_t initrd_ops = {
    .read    = initrd_read_fn,
    .write   = (void *)0,
    .close   = initrd_close_fn,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = initrd_stat_fn,
};

/* ── Directory entry type and static directory listings ────────────────── */

typedef struct { const char *name; uint8_t type; } dir_entry_t;

static const dir_entry_t s_root_entries[] = {
    { "etc", 4 }, { "bin", 4 }, { (const char *)0, 0 }
};
static const dir_entry_t s_etc_entries[] = {
    { "motd", 8 }, { (const char *)0, 0 }
};
static const dir_entry_t s_bin_entries[] = {
    { "sh",    8 }, { "ls",    8 }, { "cat",   8 }, { "echo",  8 },
    { "pwd",   8 }, { "uname", 8 }, { "clear",  8 }, { "true",  8 },
    { "false", 8 }, { "wc",    8 }, { "grep",  8 }, { "sort",  8 },
    { (const char *)0, 0 }
};

static int
dir_readdir_fn(void *priv, uint64_t index, char *name_out, uint8_t *type_out)
{
    const dir_entry_t *entries = (const dir_entry_t *)priv;
    uint64_t i = 0;
    while (entries[i].name && i < index) i++;
    if (!entries[i].name) return -1;
    const char *n = entries[i].name;
    uint64_t j;
    for (j = 0; n[j]; j++) name_out[j] = n[j];
    name_out[j] = '\0';
    *type_out = entries[i].type;
    return 0;
}

static int
dir_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)buf; (void)off; (void)len;
    return -EISDIR;
}

static void
dir_close_fn(void *priv)
{
    (void)priv;
}

static const vfs_ops_t dir_ops = {
    .read    = dir_read_fn,
    .write   = (void *)0,
    .close   = dir_close_fn,
    .readdir = dir_readdir_fn,
    .dup     = (void *)0,
    .stat    = dir_stat_fn,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

void
initrd_register(void)
{
    printk("[INITRD] OK: %u files registered\n", s_nfiles);
}

int
initrd_open(const char *path, vfs_file_t *out)
{
    /* Check for directory paths first */
    {
        const char *dirs[3] = { "/", "/etc", "/bin" };
        const dir_entry_t *dir_tables[3] = {
            s_root_entries, s_etc_entries, s_bin_entries
        };
        uint32_t d;
        for (d = 0; d < 3; d++) {
            const char *a = path, *b = dirs[d];
            while (*a && *b && *a == *b) { a++; b++; }
            if (*a == *b) {
                out->ops    = &dir_ops;
                out->priv   = (void *)dir_tables[d];
                out->offset = 0;
                out->size   = 0;
                return 0;
            }
        }
    }

    /* Regular file lookup */
    uint32_t i;
    for (i = 0; s_files[i].name != (const char *)0; i++) {
        const char *a = path, *b = s_files[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            out->ops    = &initrd_ops;
            out->priv   = (void *)&s_files[i];
            out->offset = 0;
            out->size   = entry_size(&s_files[i]);
            return 0;
        }
    }
    return -2; /* ENOENT */
}

const void *
initrd_get_data(const vfs_file_t *f)
{
    if (!f->priv) return (const void *)0;
    return (const void *)((const initrd_entry_t *)f->priv)->data;
}

uint32_t
initrd_get_size(const vfs_file_t *f)
{
    if (!f->priv) return 0;
    return entry_size((const initrd_entry_t *)f->priv);
}

/*
 * initrd_stat_entry — fill *out with stat for the initrd file at path.
 * Returns 0 if found, -2 (ENOENT) if not found.
 * Used by vfs_stat_path to avoid re-opening the file.
 */
int
initrd_stat_entry(const char *path, k_stat_t *out)
{
    uint32_t i;
    for (i = 0; s_files[i].name != (const char *)0; i++) {
        const char *a = path, *b = s_files[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            return initrd_stat_fn((void *)&s_files[i], out);
        }
    }
    return -2; /* ENOENT */
}
