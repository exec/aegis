#include "initrd.h"
#include "kbd_vfs.h"
#include "random.h"
#include "printk.h"
#include <stdint.h>

#ifndef EISDIR
#define EISDIR 21
#endif

/* /etc/motd content — displayed by stsh on login.
 * The filter keeps only lines starting with '['; content not matching is
 * silently dropped from the serial diff. */
static const char s_banner[] =
    "\n"
    " _______ _______  ______ _____ _______\n"
    " |_____| |______ |  ____   |   |______\n"
    " |     | |______ |_____| __|__ ______|\n"
    "\n"
    " WARNING: This system is restricted to authorized users.\n"
    " All activity is monitored and logged. Unauthorized access\n"
    " will be investigated and may result in prosecution.\n"
    "\n";

static const char s_banner_net[] =
    "\n"
    " _______ _______  ______ _____ _______\n"
    " |_____| |______ |  ____   |   |______\n"
    " |     | |______ |_____| __|__ ______|\n"
    "\n"
    " WARNING: This system is restricted to authorized users.\n"
    " All connections are monitored and logged.\n"
    "\n";

static const char s_motd[] =
    "\n"
    " _______ _______  ______ _____ _______\n"
    " |_____| |______ |  ____   |   |______\n"
    " |     | |______ |_____| __|__ ______|\n"
    "\n"
    " Aegis 1.0.0 \"Ambient Argus\"\n"
    "\n";
static const char s_passwd[] = "root:x:0:0:root:/root:/bin/stsh\n";
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
    "104.18.27.120 example.com\n";

/* Default vigil service config — allows vigil to find its getty service even
 * on a disk that was built before vigil was added (or on no disk at all).
 * run: direct path so start_service execs login without a shell intermediary,
 * ensuring exec_caps (AUTH) are applied to login, not consumed by sh. */
static const char s_vigil_run[]    = "/bin/login\n";
static const char s_vigil_policy[] = "respawn\nmax_restarts=5\n";
static const char s_vigil_caps[]   = "AUTH\nCAP_GRANT\nCAP_DELEGATE\nCAP_QUERY\n";

/* httpd vigil service config — binds :80, serves HTTP for socket API tests. */
static const char s_httpd_run[]    = "/bin/httpd\n";
static const char s_httpd_policy[] = "respawn\nmax_restarts=5\n";
static const char s_httpd_caps[]   = "NET_SOCKET VFS_OPEN VFS_READ\n";

/* dhcp vigil service config */
static const char s_dhcp_run[]    = "/bin/dhcp\n";
static const char s_dhcp_policy[] = "oneshot\n";
static const char s_dhcp_caps[]   = "NET_ADMIN NET_SOCKET\n";

/* chronos vigil service config — NTP time sync daemon */
static const char s_chronos_run[]    = "/bin/chronos\n";
static const char s_chronos_policy[] = "oneshot\n";
static const char s_chronos_caps[]   = "NET_SOCKET\n";

/* Capability policy files — /etc/aegis/caps.d/<binary>
 * The kernel's cap policy table reads these at execve time to determine
 * which capabilities to grant. Format: "tier CAP1 CAP2 ...\n" per line.
 * "service" tier: granted unconditionally at execve.
 * "admin" tier: granted only if the session is authenticated. */
static const char s_cap_login[]     = "service AUTH SETUID\n";
static const char s_cap_bastion[]   = "service AUTH FB SETUID\n";
static const char s_cap_httpd[]     = "service NET_SOCKET\n";
static const char s_cap_dhcp[]      = "service NET_SOCKET NET_ADMIN\n";
static const char s_cap_stsh[]      = "admin DISK_ADMIN POWER CAP_DELEGATE CAP_QUERY\nadmin PROC_READ\n";
static const char s_cap_lumen[]     = "service FB THREAD_CREATE\n";
static const char s_cap_installer[] = "admin DISK_ADMIN AUTH\n";

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
static const unsigned int s_chronos_run_size    = sizeof(s_chronos_run)    - 1;
static const unsigned int s_chronos_policy_size = sizeof(s_chronos_policy) - 1;
static const unsigned int s_chronos_caps_size   = sizeof(s_chronos_caps)   - 1;
static const unsigned int s_cap_login_size      = sizeof(s_cap_login)      - 1;
static const unsigned int s_cap_bastion_size    = sizeof(s_cap_bastion)    - 1;
static const unsigned int s_cap_httpd_size      = sizeof(s_cap_httpd)      - 1;
static const unsigned int s_cap_dhcp_size       = sizeof(s_cap_dhcp)       - 1;
static const unsigned int s_cap_stsh_size       = sizeof(s_cap_stsh)       - 1;
static const unsigned int s_cap_lumen_size      = sizeof(s_cap_lumen)      - 1;
static const unsigned int s_cap_installer_size  = sizeof(s_cap_installer)  - 1;

/* Binary blobs embedded into the kernel image.
 * Size = end - start (computed at open time).
 *
 * Boot-critical static binaries in initrd: login, vigil, shell, echo, cat, ls.
 * All other user binaries are dynamically linked and live on the ext2 disk.
 *
 * x86_64 convention: objcopy --input binary produces
 *     _binary_<name>_bin_{start,end}
 * for each user/bin/<name>.bin blob (see top-level Makefile).
 *
 * aarch64 convention: each binary is embedded as a C array in
 *     kernel/arch/arm64/<name>_arm64_bin.c
 * which defines
 *     const unsigned char <name>_elf[];
 *     const unsigned int  <name>_elf_len;
 *
 * We pick the right set at compile time via __aarch64__ and build a
 * parallel `_start`/`_end` pointer pair for each entry so the rest of
 * this file can iterate a single uniform table regardless of arch.
 *
 * Note on /bin/vigil: arm64 does not currently build a separate `vigil`
 * user binary. `init_elf` is the arm64 PID 1 init and plays vigil's role
 * (see kernel/proc/proc.c where proc_spawn_init() feeds init_elf to the
 * ELF loader under __aarch64__). Until vigil is cross-built for arm64,
 * /bin/vigil in the initrd points at the same init_elf blob. */
#ifdef __aarch64__
extern const unsigned char init_elf[];
extern const unsigned int  init_elf_len;
extern const unsigned char login_elf[];
extern const unsigned int  login_elf_len;
extern const unsigned char shell_elf[];
extern const unsigned int  shell_elf_len;
extern const unsigned char echo_elf[];
extern const unsigned int  echo_elf_len;
extern const unsigned char cat_elf[];
extern const unsigned int  cat_elf_len;
extern const unsigned char ls_elf[];
extern const unsigned int  ls_elf_len;

/* On arm64 the blob `end` pointers are NOT compile-time constants
 * (`*_elf_len` is a runtime-initialized extern `const unsigned int`).
 * Initialize `end` to NULL and patch at initrd_register() time via
 * s_arm64_blob_fixups[] below. /bin/vigil aliases init_elf. */
#define LOGIN_START  (login_elf)
#define LOGIN_END    ((const unsigned char *)0)
#define VIGIL_START  (init_elf)
#define VIGIL_END    ((const unsigned char *)0)
#define SHELL_START  (shell_elf)
#define SHELL_END    ((const unsigned char *)0)
#define ECHO_START   (echo_elf)
#define ECHO_END     ((const unsigned char *)0)
#define CAT_START    (cat_elf)
#define CAT_END      ((const unsigned char *)0)
#define LS_START     (ls_elf)
#define LS_END       ((const unsigned char *)0)
#else
extern const unsigned char _binary_login_bin_start[];
extern const unsigned char _binary_login_bin_end[];
extern const unsigned char _binary_vigil_bin_start[];
extern const unsigned char _binary_vigil_bin_end[];
extern const unsigned char _binary_shell_bin_start[];
extern const unsigned char _binary_shell_bin_end[];
extern const unsigned char _binary_echo_bin_start[];
extern const unsigned char _binary_echo_bin_end[];
extern const unsigned char _binary_cat_bin_start[];
extern const unsigned char _binary_cat_bin_end[];
extern const unsigned char _binary_ls_bin_start[];
extern const unsigned char _binary_ls_bin_end[];

#define LOGIN_START  _binary_login_bin_start
#define LOGIN_END    _binary_login_bin_end
#define VIGIL_START  _binary_vigil_bin_start
#define VIGIL_END    _binary_vigil_bin_end
#define SHELL_START  _binary_shell_bin_start
#define SHELL_END    _binary_shell_bin_end
#define ECHO_START   _binary_echo_bin_start
#define ECHO_END     _binary_echo_bin_end
#define CAT_START    _binary_cat_bin_start
#define CAT_END      _binary_cat_bin_end
#define LS_START     _binary_ls_bin_start
#define LS_END       _binary_ls_bin_end
#endif

/* initrd_entry_t — each entry holds a path, a pointer to file data, and a
 * pointer to the file's size variable (link-time value from objcopy/bin2c).
 * Using a pointer-to-size avoids the "initializer element is not constant"
 * error that arises when placing extern uint variables in static initializers. */
typedef struct {
    const char          *name;
    const unsigned char *start;  /* blob start (_binary_<prog>_start or inline string) */
    const unsigned char *end;    /* blob end (_binary_<prog>_end or start + strlen) */
} initrd_entry_t;

/* On aarch64 the blob end pointers are not compile-time constants
 * (`login_elf_len` is an extern `const unsigned int`, not a #define or
 * sizeof), so this table cannot be `const` on arm64 — the blob `end`
 * fields are patched in at initrd_register() time. On x86 the end
 * pointers are linker-provided array symbols, so the table can stay
 * fully static-initialized. */
#ifdef __aarch64__
static initrd_entry_t s_files[] = {
#else
static const initrd_entry_t s_files[] = {
#endif
    { "/etc/motd",       (const unsigned char *)s_motd,       (const unsigned char *)s_motd + sizeof(s_motd) - 1 },
    { "/etc/banner",     (const unsigned char *)s_banner,     (const unsigned char *)s_banner + sizeof(s_banner) - 1 },
    { "/etc/banner.net", (const unsigned char *)s_banner_net, (const unsigned char *)s_banner_net + sizeof(s_banner_net) - 1 },
    { "/bin/login",   LOGIN_START, LOGIN_END },
    { "/bin/vigil",   VIGIL_START, VIGIL_END },
    { "/bin/sh",      SHELL_START, SHELL_END },
    { "/bin/echo",    ECHO_START,  ECHO_END  },
    { "/bin/cat",     CAT_START,   CAT_END   },
    { "/bin/ls",      LS_START,    LS_END    },
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
    { "/etc/vigil/services/chronos/run", (const unsigned char *)s_chronos_run, (const unsigned char *)s_chronos_run + s_chronos_run_size },
    { "/etc/vigil/services/chronos/policy", (const unsigned char *)s_chronos_policy, (const unsigned char *)s_chronos_policy + s_chronos_policy_size },
    { "/etc/vigil/services/chronos/caps", (const unsigned char *)s_chronos_caps, (const unsigned char *)s_chronos_caps + s_chronos_caps_size },
    { "/etc/aegis/caps.d/login", (const unsigned char *)s_cap_login, (const unsigned char *)s_cap_login + s_cap_login_size },
    { "/etc/aegis/caps.d/bastion", (const unsigned char *)s_cap_bastion, (const unsigned char *)s_cap_bastion + s_cap_bastion_size },
    { "/etc/aegis/caps.d/httpd", (const unsigned char *)s_cap_httpd, (const unsigned char *)s_cap_httpd + s_cap_httpd_size },
    { "/etc/aegis/caps.d/dhcp", (const unsigned char *)s_cap_dhcp, (const unsigned char *)s_cap_dhcp + s_cap_dhcp_size },
    { "/etc/aegis/caps.d/stsh", (const unsigned char *)s_cap_stsh, (const unsigned char *)s_cap_stsh + s_cap_stsh_size },
    { "/etc/aegis/caps.d/lumen", (const unsigned char *)s_cap_lumen, (const unsigned char *)s_cap_lumen + s_cap_lumen_size },
    { "/etc/aegis/caps.d/installer", (const unsigned char *)s_cap_installer, (const unsigned char *)s_cap_installer + s_cap_installer_size },
    { (const char *)0, (const unsigned char *)0, (const unsigned char *)0 }  /* sentinel */
};


static const uint32_t s_nfiles = 32;

/* Helper: return file size for an entry. */
static uint32_t
entry_size(const initrd_entry_t *e)
{
    return (uint32_t)(e->end - e->start);
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
    /* /etc/shadow must be 0640 root:root — not world-readable.
     * All other initrd files are executables or config (0555). */
    if (e->name[0]=='/' && e->name[1]=='e' && e->name[2]=='t' &&
        e->name[3]=='c' && e->name[4]=='/' && e->name[5]=='s' &&
        e->name[6]=='h' && e->name[7]=='a' && e->name[8]=='d' &&
        e->name[9]=='o' && e->name[10]=='w' && e->name[11]=='\0')
        st->st_mode = S_IFREG | 0640;
    else
        st->st_mode = S_IFREG | 0555;
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
    .poll    = (void *)0,
};

/* ── Directory entry type and static directory listings ────────────────── */

typedef struct { const char *name; uint8_t type; } dir_entry_t;

static const dir_entry_t s_dev_entries[] = {
    { "tty",     8 }, { "urandom", 8 }, { "random", 8 }, { "mouse", 8 },
    { (const char *)0, 0 }
};
static const dir_entry_t s_root_dir_entries[] = {
    { (const char *)0, 0 }   /* /root is empty */
};
static const dir_entry_t s_root_entries[] = {
    { "etc", 4 }, { "bin", 4 }, { "dev", 4 }, { "lib", 4 }, { "root", 4 },
    { "tmp", 4 }, { "run", 4 }, { "proc", 4 },
    { (const char *)0, 0 }
};
static const dir_entry_t s_etc_entries[] = {
    { "motd", 8 }, { "banner", 8 }, { "banner.net", 8 }, { "passwd", 8 }, { "shadow", 8 }, { "profile", 8 },
    { "vigil", 4 }, { "aegis", 4 }, { (const char *)0, 0 }
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
static const dir_entry_t s_aegis_entries[] = {
    { "caps.d", 4 }, { (const char *)0, 0 }
};
static const dir_entry_t s_aegis_capsd_entries[] = {
    { "login", 8 }, { "bastion", 8 }, { "httpd", 8 }, { "dhcp", 8 },
    { "stsh", 8 }, { "lumen", 8 }, { "installer", 8 }, { (const char *)0, 0 }
};
/* s_bin_entries removed — /bin directory listing now handled by ext2.
 * Individual files (/bin/login, /bin/vigil) are still found via initrd_open
 * by name, but `ls /bin` falls through to ext2_readdir which shows all
 * binaries on disk. */

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
    .poll    = (void *)0,
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
    .poll    = (void *)0,
};

static vfs_file_t s_urandom_file = {
    .ops    = &s_urandom_ops,
    .priv   = (void *)0,
    .offset = 0,
    .size   = 0,
};

/* ── /dev/mouse VFS device ──────────────────────────────────────────────── */

#include "usb_mouse.h"

static int
mouse_read_fn(void *priv, void *buf, uint64_t off, uint64_t len)
{
    (void)priv; (void)off;
    uint32_t count = 0;
    uint32_t max_events = (uint32_t)(len / sizeof(mouse_event_t));

    if (max_events == 0) return 0;

    /* Non-blocking: drain available events only */
    mouse_event_t evt;
    while (count < max_events && mouse_poll(&evt)) {
        __builtin_memcpy((uint8_t *)buf + count * sizeof(mouse_event_t),
                         &evt, sizeof(mouse_event_t));
        count++;
    }

    if (count == 0) return -11;  /* -EAGAIN */
    return (int)(count * sizeof(mouse_event_t));
}

static void
mouse_close_fn(void *priv)
{
    (void)priv;
}

static int
mouse_stat_fn(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_mode  = S_IFCHR | 0444;
    st->st_ino   = 8;
    st->st_rdev  = makedev(13, 0);
    st->st_dev   = 1;
    st->st_nlink = 1;
    return 0;
}

static const vfs_ops_t s_mouse_ops = {
    .read    = mouse_read_fn,
    .write   = (void *)0,
    .close   = mouse_close_fn,
    .readdir = (void *)0,
    .dup     = (void *)0,
    .stat    = mouse_stat_fn,
    .poll    = (void *)0,
};

static vfs_file_t s_mouse_file = {
    .ops    = &s_mouse_ops,
    .priv   = (void *)0,
    .offset = 0,
    .size   = 0,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

void
initrd_register(void)
{
#ifdef __aarch64__
    /* Patch blob `end` pointers that could not be statically initialized
     * (see comment above s_files[]). Walk s_files and fix up each entry
     * whose `start` matches one of the arm64 blob arrays. */
    uint32_t i;
    for (i = 0; s_files[i].name != (const char *)0; i++) {
        if (s_files[i].start == login_elf)
            s_files[i].end = login_elf + login_elf_len;
        else if (s_files[i].start == init_elf)
            s_files[i].end = init_elf + init_elf_len;
        else if (s_files[i].start == shell_elf)
            s_files[i].end = shell_elf + shell_elf_len;
        else if (s_files[i].start == echo_elf)
            s_files[i].end = echo_elf + echo_elf_len;
        else if (s_files[i].start == cat_elf)
            s_files[i].end = cat_elf + cat_elf_len;
        else if (s_files[i].start == ls_elf)
            s_files[i].end = ls_elf + ls_elf_len;
    }
#endif
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

    /* /dev/mouse: USB HID mouse event device */
    {
        const char *a = path, *b = "/dev/mouse";
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) {
            *out = s_mouse_file;
            return 0;
        }
    }

    /* Check for directory paths first */
    {
        const char *dirs[12] = {
            "/", "/etc", "/dev",
            "/etc/vigil", "/etc/vigil/services", "/etc/vigil/services/getty",
            "/etc/vigil/services/httpd",
            "/etc/vigil/services/dhcp",
            "/root",
            "/etc/aegis",
            "/etc/aegis/caps.d",
            (const char *)0
        };
        const dir_entry_t *dir_tables[12] = {
            s_root_entries, s_etc_entries, s_dev_entries,
            s_vigil_entries, s_vigil_services_entries, s_vigil_getty_entries,
            s_vigil_httpd_entries,
            s_vigil_dhcp_entries,
            s_root_dir_entries,
            s_aegis_entries,
            s_aegis_capsd_entries,
            (const dir_entry_t *)0
        };
        uint32_t d;
        for (d = 0; d < 11; d++) {
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
