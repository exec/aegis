#include "initrd.h"
#include "kbd_vfs.h"
#include "random.h"
#include "printk.h"
#include <stdint.h>

#ifndef EISDIR
#define EISDIR 21
#endif

/* /etc/motd content — starts with '[' so it survives the make test ANSI filter.
 * The filter keeps only lines starting with '['; content not matching is
 * silently dropped from the serial diff. */
static const char s_motd[] = "[MOTD] Welcome to Aegis\n";
static const char s_passwd[] = "root:x:0:0:root:/root:/bin/oksh\n";
static const char s_shadow[] =
    "root:$6$5a3b9c1d2e4f6789$fvwyIjdmyvB59hifGMRFrcwhBb4cH0.3nRy2j2LpCk."
    "aNIFNyvYQJ36Bsl94miFbD/JHICz8O1dXoegZ0OmOg.:19000:0:99999:7:::\n";
static const char s_profile[] =
    "PS1='root@aegis:${PWD:-/}# '\n"
    "export PS1\n"
    "PATH=/bin\n"
    "export PATH\n";
static const char s_hosts[] =
    "127.0.0.1 localhost\n"
    "10.0.2.15 aegis\n"
    "104.18.26.120 example.com\n";

/* Default vigil service config — allows vigil to find its getty service even
 * on a disk that was built before vigil was added (or on no disk at all).
 * run: direct path so start_service execs login without a shell intermediary,
 * ensuring exec_caps (AUTH) are applied to login, not consumed by sh. */
static const char s_vigil_run[]    = "/bin/login\n";
static const char s_vigil_policy[] = "respawn\nmax_restarts=5\n";
static const char s_vigil_caps[]   = "AUTH\n";

/* httpd vigil service config — binds :80, serves HTTP for socket API tests. */
static const char s_httpd_run[]    = "/bin/httpd\n";
static const char s_httpd_policy[] = "respawn\nmax_restarts=5\n";
static const char s_httpd_caps[]   = "NET_SOCKET VFS_OPEN VFS_READ\n";

/* dhcp vigil service config */
static const char s_dhcp_run[]    = "/bin/dhcp\n";
static const char s_dhcp_policy[] = "respawn\nmax_restarts=10\n";
static const char s_dhcp_caps[]   = "NET_ADMIN NET_SOCKET\n";

/* Compile-time size constants for static string entries. */
static const unsigned int s_hosts_size         = sizeof(s_hosts)         - 1;
static const unsigned int s_passwd_size        = sizeof(s_passwd)        - 1;
static const unsigned int s_shadow_size        = sizeof(s_shadow)        - 1;
static const unsigned int s_profile_size       = sizeof(s_profile)       - 1;
static const unsigned int s_vigil_run_size     = sizeof(s_vigil_run)     - 1;
static const unsigned int s_vigil_policy_size  = sizeof(s_vigil_policy)  - 1;
static const unsigned int s_vigil_caps_size    = sizeof(s_vigil_caps)    - 1;
static const unsigned int s_httpd_run_size     = sizeof(s_httpd_run)     - 1;
static const unsigned int s_httpd_policy_size  = sizeof(s_httpd_policy)  - 1;
static const unsigned int s_httpd_caps_size    = sizeof(s_httpd_caps)    - 1;
static const unsigned int s_dhcp_run_size    = sizeof(s_dhcp_run)    - 1;
static const unsigned int s_dhcp_policy_size = sizeof(s_dhcp_policy) - 1;
static const unsigned int s_dhcp_caps_size   = sizeof(s_dhcp_caps)   - 1;

/* Binary blobs embedded via objcopy --input binary.
 * Symbols: _binary_<name>_start, _binary_<name>_end.
 * Size = end - start (computed at open time). */
extern const unsigned char _binary_shell_bin_start[];
extern const unsigned char _binary_shell_bin_end[];
extern const unsigned char _binary_ls_bin_start[];
extern const unsigned char _binary_ls_bin_end[];
extern const unsigned char _binary_cat_bin_start[];
extern const unsigned char _binary_cat_bin_end[];
extern const unsigned char _binary_echo_bin_start[];
extern const unsigned char _binary_echo_bin_end[];
extern const unsigned char _binary_pwd_bin_start[];
extern const unsigned char _binary_pwd_bin_end[];
extern const unsigned char _binary_uname_bin_start[];
extern const unsigned char _binary_uname_bin_end[];
extern const unsigned char _binary_clear_bin_start[];
extern const unsigned char _binary_clear_bin_end[];
extern const unsigned char _binary_true_bin_start[];
extern const unsigned char _binary_true_bin_end[];
extern const unsigned char _binary_false_bin_start[];
extern const unsigned char _binary_false_bin_end[];
extern const unsigned char _binary_wc_bin_start[];
extern const unsigned char _binary_wc_bin_end[];
extern const unsigned char _binary_grep_bin_start[];
extern const unsigned char _binary_grep_bin_end[];
extern const unsigned char _binary_sort_bin_start[];
extern const unsigned char _binary_sort_bin_end[];
extern const unsigned char _binary_mkdir_bin_start[];
extern const unsigned char _binary_mkdir_bin_end[];
extern const unsigned char _binary_touch_bin_start[];
extern const unsigned char _binary_touch_bin_end[];
extern const unsigned char _binary_rm_bin_start[];
extern const unsigned char _binary_rm_bin_end[];
extern const unsigned char _binary_cp_bin_start[];
extern const unsigned char _binary_cp_bin_end[];
extern const unsigned char _binary_mv_bin_start[];
extern const unsigned char _binary_mv_bin_end[];
extern const unsigned char _binary_whoami_bin_start[];
extern const unsigned char _binary_whoami_bin_end[];
extern const unsigned char _binary_oksh_bin_start[];
extern const unsigned char _binary_oksh_bin_end[];
extern const unsigned char _binary_login_bin_start[];
extern const unsigned char _binary_login_bin_end[];
extern const unsigned char _binary_vigil_bin_start[];
extern const unsigned char _binary_vigil_bin_end[];
extern const unsigned char _binary_vigictl_bin_start[];
extern const unsigned char _binary_vigictl_bin_end[];
extern const unsigned char _binary_httpd_bin_start[];
extern const unsigned char _binary_httpd_bin_end[];
extern const unsigned char _binary_dhcp_bin_start[];
extern const unsigned char _binary_dhcp_bin_end[];

/* initrd_entry_t — each entry holds a path, a pointer to file data, and a
 * pointer to the file's size variable (link-time value from objcopy/bin2c).
 * Using a pointer-to-size avoids the "initializer element is not constant"
 * error that arises when placing extern uint variables in static initializers. */
typedef struct {
    const char          *name;
    const unsigned char *start;  /* blob start (_binary_<prog>_start or inline string) */
    const unsigned char *end;    /* blob end (_binary_<prog>_end or start + strlen) */
} initrd_entry_t;

static const initrd_entry_t s_files[] = {
    { "/etc/motd",  (const unsigned char *)s_motd, (const unsigned char *)s_motd + sizeof(s_motd) - 1 },
    { "/bin/sh",      _binary_shell_bin_start,     _binary_shell_bin_end },
    { "/bin/ls",      _binary_ls_bin_start,        _binary_ls_bin_end },
    { "/bin/cat",     _binary_cat_bin_start,       _binary_cat_bin_end },
    { "/bin/echo",    _binary_echo_bin_start,      _binary_echo_bin_end },
    { "/bin/pwd",     _binary_pwd_bin_start,       _binary_pwd_bin_end },
    { "/bin/uname",   _binary_uname_bin_start,     _binary_uname_bin_end },
    { "/bin/clear",   _binary_clear_bin_start,     _binary_clear_bin_end },
    { "/bin/true",    _binary_true_bin_start,     _binary_true_bin_end },
    { "/bin/false",   _binary_false_bin_start,    _binary_false_bin_end },
    { "/bin/wc",      _binary_wc_bin_start,        _binary_wc_bin_end },
    { "/bin/grep",    _binary_grep_bin_start,      _binary_grep_bin_end },
    { "/bin/sort",    _binary_sort_bin_start,      _binary_sort_bin_end },
    { "/bin/mkdir",   _binary_mkdir_bin_start,     _binary_mkdir_bin_end },
    { "/bin/touch",   _binary_touch_bin_start,     _binary_touch_bin_end },
    { "/bin/rm",      _binary_rm_bin_start,        _binary_rm_bin_end },
    { "/bin/cp",      _binary_cp_bin_start,        _binary_cp_bin_end },
    { "/bin/mv",      _binary_mv_bin_start,        _binary_mv_bin_end },
    { "/bin/whoami",  _binary_whoami_bin_start,    _binary_whoami_bin_end },
    { "/bin/oksh",    _binary_oksh_bin_start,      _binary_oksh_bin_end },
    { "/bin/login",   _binary_login_bin_start,     _binary_login_bin_end },
    { "/bin/vigil",   _binary_vigil_bin_start,     _binary_vigil_bin_end },
    { "/bin/vigictl", _binary_vigictl_bin_start,   _binary_vigictl_bin_end },
    { "/bin/httpd",   _binary_httpd_bin_start,     _binary_httpd_bin_end },
    { "/bin/dhcp",    _binary_dhcp_bin_start,      _binary_dhcp_bin_end },
    { "/etc/vigil/services/dhcp/run", (const unsigned char *)s_dhcp_run, (const unsigned char *)s_dhcp_run + s_dhcp_run_size },
    { "/etc/vigil/services/dhcp/policy", (const unsigned char *)s_dhcp_policy, (const unsigned char *)s_dhcp_policy + s_dhcp_policy_size },
    { "/etc/vigil/services/dhcp/caps", (const unsigned char *)s_dhcp_caps, (const unsigned char *)s_dhcp_caps + s_dhcp_caps_size },
    { "/etc/hosts",  (const unsigned char *)s_hosts,  (const unsigned char *)s_hosts + s_hosts_size },
    { "/etc/passwd", (const unsigned char *)s_passwd, (const unsigned char *)s_passwd + s_passwd_size },
    { "/etc/shadow", (const unsigned char *)s_shadow, (const unsigned char *)s_shadow + s_shadow_size },
    { "/etc/profile", (const unsigned char *)s_profile, (const unsigned char *)s_profile + s_profile_size },
    { "/etc/vigil/services/getty/run", (const unsigned char *)s_vigil_run, (const unsigned char *)s_vigil_run + s_vigil_run_size },
    { "/etc/vigil/services/getty/policy", (const unsigned char *)s_vigil_policy, (const unsigned char *)s_vigil_policy + s_vigil_policy_size },
    { "/etc/vigil/services/getty/caps", (const unsigned char *)s_vigil_caps, (const unsigned char *)s_vigil_caps + s_vigil_caps_size },
    { "/etc/vigil/services/httpd/run", (const unsigned char *)s_httpd_run, (const unsigned char *)s_httpd_run + s_httpd_run_size },
    { "/etc/vigil/services/httpd/policy", (const unsigned char *)s_httpd_policy, (const unsigned char *)s_httpd_policy + s_httpd_policy_size },
    { "/etc/vigil/services/httpd/caps", (const unsigned char *)s_httpd_caps, (const unsigned char *)s_httpd_caps + s_httpd_caps_size },
    { (const char *)0, (const unsigned char *)0, (const unsigned char *)0 }  /* sentinel */
};


static const uint32_t s_nfiles = 38;

/* Helper: return file size for an entry. */
static uint32_t
entry_size(const initrd_entry_t *e)
{
    return (uint32_t)(e->end - e->start);
}








void
initrd_iter_etc(initrd_etc_cb_t cb, void *ud)
{
    uint32_t i;
    for (i = 0; s_files[i].name; i++) {
        const initrd_entry_t *e = &s_files[i];
        /* Match paths starting with "/etc/" — skip "/etc" without trailing slash */
        if (e->name[0] != '/' || e->name[1] != 'e' ||
            e->name[2] != 't' || e->name[3] != 'c' || e->name[4] != '/')
            continue;
        cb(e->name + 5,  /* short name: skip "/etc/" prefix */
           (const uint8_t *)e->start,
           entry_size(e),
           ud);
    }
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
    __builtin_memcpy(buf, e->start + off, len);
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
    st->st_mode    = S_IFREG | 0555;
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

static const dir_entry_t s_dev_entries[] = {
    { "tty",     8 }, { "urandom", 8 }, { "random", 8 },
    { (const char *)0, 0 }
};
static const dir_entry_t s_root_dir_entries[] = {
    { (const char *)0, 0 }   /* /root is empty */
};
static const dir_entry_t s_root_entries[] = {
    { "etc", 4 }, { "bin", 4 }, { "dev", 4 }, { "root", 4 },
    { (const char *)0, 0 }
};
static const dir_entry_t s_etc_entries[] = {
    { "motd", 8 }, { "passwd", 8 }, { "shadow", 8 }, { "profile", 8 },
    { "vigil", 4 }, { (const char *)0, 0 }
};
static const dir_entry_t s_vigil_entries[] = {
    { "services", 4 }, { (const char *)0, 0 }
};
static const dir_entry_t s_vigil_services_entries[] = {
    { "getty", 4 }, { "httpd", 4 }, { "dhcp", 4 }, { (const char *)0, 0 }
};
static const dir_entry_t s_vigil_getty_entries[] = {
    { "run", 8 }, { "policy", 8 }, { "caps", 8 }, { (const char *)0, 0 }
};
static const dir_entry_t s_vigil_httpd_entries[] = {
    { "run", 8 }, { "policy", 8 }, { "caps", 8 }, { (const char *)0, 0 }
};
static const dir_entry_t s_vigil_dhcp_entries[] = {
    { "run", 8 }, { "policy", 8 }, { "caps", 8 }, { (const char *)0, 0 }
};
static const dir_entry_t s_bin_entries[] = {
    { "sh",     8 }, { "ls",     8 }, { "cat",    8 }, { "echo",   8 },
    { "pwd",    8 }, { "uname",  8 }, { "clear",  8 }, { "true",   8 },
    { "false",  8 }, { "wc",     8 }, { "grep",   8 }, { "sort",   8 },
    { "mkdir",  8 }, { "touch",  8 }, { "rm",     8 }, { "cp",     8 },
    { "mv",     8 }, { "whoami", 8 }, { "oksh",   8 }, { "login",  8 },
    { "vigil",  8 }, { "vigictl", 8 }, { "httpd", 8 }, { "dhcp",   8 },
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

/* ── /dev/urandom VFS device ────────────────────────────────────────────── */

static int
urandom_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)off;
    if (len > 4096) len = 4096;
    random_get_bytes(buf, (size_t)len);
    return (int)len;
}

static int
urandom_write_fn(void *priv, const void *buf, uint64_t len)
{
    /* Writes to urandom are accepted but not mixed into the pool.
     * buf is a user-space pointer (SMAP-protected); a proper implementation
     * would copy_from_user first. For now, just acknowledge the write. */
    (void)priv; (void)buf;
    return (int)(len > 256 ? 256 : len);
}

static void
urandom_close_fn(void *priv)
{
    (void)priv;
}

static int
urandom_stat_fn(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode  = S_IFCHR | 0666;
    st->st_ino   = 5;
    st->st_rdev  = makedev(1, 9);   /* Linux: /dev/urandom = 1:9 */
    st->st_dev   = 1;
    st->st_nlink = 1;
    return 0;
}

static const vfs_ops_t s_urandom_ops = {
    .read    = urandom_read_fn,
    .write   = urandom_write_fn,
    .close   = urandom_close_fn,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = urandom_stat_fn,
};

static vfs_file_t s_urandom_file = {
    .ops    = &s_urandom_ops,
    .priv   = (void *)0,
    .offset = 0,
    .size   = 0,
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
    /* /dev/tty: return the keyboard VFS singleton directly */
    {
        const char *a = path, *b = "/dev/tty";
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            vfs_file_t *tty = kbd_vfs_open();
            *out = *tty;
            return 0;
        }
    }

    /* /dev/urandom and /dev/random: CSPRNG device */
    {
        const char *a = path, *b = "/dev/urandom";
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            *out = s_urandom_file;
            return 0;
        }
    }
    {
        const char *a = path, *b = "/dev/random";
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            *out = s_urandom_file;  /* same backing — modern Linux semantics */
            return 0;
        }
    }

    /* Check for directory paths first */
    {
        const char *dirs[11] = {
            "/", "/etc", "/bin", "/dev",
            "/etc/vigil", "/etc/vigil/services", "/etc/vigil/services/getty",
            "/etc/vigil/services/httpd",
            "/etc/vigil/services/dhcp",
            "/root",
            (const char *)0
        };
        const dir_entry_t *dir_tables[11] = {
            s_root_entries, s_etc_entries, s_bin_entries, s_dev_entries,
            s_vigil_entries, s_vigil_services_entries, s_vigil_getty_entries,
            s_vigil_httpd_entries,
            s_vigil_dhcp_entries,
            s_root_dir_entries,
            (const dir_entry_t *)0
        };
        uint32_t d;
        for (d = 0; d < 10; d++) {
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
    const initrd_entry_t *e = (const initrd_entry_t *)f->priv; return (const void *)e->start;
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
