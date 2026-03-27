/* kernel/fs/procfs.c — /proc virtual filesystem
 *
 * Generate-on-open design: opening a /proc file allocates a kva buffer,
 * generates content into it, and stores the buffer in vfs_file_t.priv.
 * read() copies from the buffer at offset; close() frees it.
 *
 * Capability gating: /proc/self/ is always permitted. /proc/[pid]/ for
 * a different pid requires CAP_KIND_PROC_READ in the caller's cap table.
 */
#include "procfs.h"
#include "vfs.h"
#include "proc.h"
#include "sched.h"
#include "pmm.h"
#include "vma.h"
#include "kva.h"
#include "cap.h"
#include "printk.h"
#include "fd_table.h"
#include <stdint.h>
#include <stddef.h>

/* ── string helpers (no libc) ──────────────────────────────────────────── */

static int
pfs_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static char *
pfs_strcpy(char *dst, const char *src)
{
    while (*src)
        *dst++ = *src++;
    return dst;
}

static char *
pfs_u64_dec(char *buf, uint64_t val)
{
    if (val == 0) {
        *buf++ = '0';
        return buf;
    }
    char tmp[20];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    while (i > 0)
        *buf++ = tmp[--i];
    return buf;
}

static char *
pfs_u64_hex(char *buf, uint64_t val, int min_digits)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[16];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = hex[val & 0xF];
            val >>= 4;
        }
    }
    while (i < min_digits)
        tmp[i++] = '0';
    while (i > 0)
        *buf++ = tmp[--i];
    return buf;
}

/* ── priv structures ───────────────────────────────────────────────────── */

typedef struct {
    char    *buf;
    uint32_t len;
    uint32_t _pad;
} procfs_file_priv_t;

typedef struct {
    uint32_t pid;     /* 0 = root /proc/ dir */
    uint8_t  is_fd;   /* 1 = /proc/[pid]/fd/ */
    uint8_t  _pad[3];
} procfs_dir_priv_t;

/* ── content generators ────────────────────────────────────────────────── */

static uint32_t
gen_maps(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    char *end = buf + bufsz - 1;
    uint32_t i;

    if (!proc->vma_table)
        return 0;

    for (i = 0; i < proc->vma_count && p < end - 80; i++) {
        vma_entry_t *v = &proc->vma_table[i];
        uint64_t vstart = v->base;
        uint64_t vend = v->base + v->len;

        /* start-end */
        p = pfs_u64_hex(p, vstart, 8);
        *p++ = '-';
        p = pfs_u64_hex(p, vend, 8);
        *p++ = ' ';

        /* perms: rwxp */
        *p++ = (v->prot & 1) ? 'r' : '-'; /* PROT_READ  = 1 */
        *p++ = (v->prot & 2) ? 'w' : '-'; /* PROT_WRITE = 2 */
        *p++ = (v->prot & 4) ? 'x' : '-'; /* PROT_EXEC  = 4 */
        *p++ = 'p';
        *p++ = ' ';

        /* offset dev inode */
        p = pfs_strcpy(p, "00000000 00:00 0");

        /* padding + name */
        switch (v->type) {
        case VMA_ELF_TEXT:
        case VMA_ELF_DATA:
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            p = pfs_strcpy(p, proc->exe_path);
            break;
        case VMA_HEAP:
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            p = pfs_strcpy(p, "[heap]");
            break;
        case VMA_STACK:
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            p = pfs_strcpy(p, "[stack]");
            break;
        case VMA_GUARD:
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            p = pfs_strcpy(p, "[guard]");
            break;
        case VMA_THREAD_STACK:
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            p = pfs_strcpy(p, "[thread_stack]");
            break;
        default:
            break;
        }
        *p++ = '\n';
    }

    *p = '\0';
    return (uint32_t)(p - buf);
}

/* basename helper — return pointer to last component after '/' */
static const char *
pfs_basename(const char *path)
{
    const char *last = path;
    while (*path) {
        if (*path == '/')
            last = path + 1;
        path++;
    }
    return last;
}

static uint32_t
gen_status(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    char *end = buf + bufsz - 1;
    (void)end;

    p = pfs_strcpy(p, "Name:\t");
    p = pfs_strcpy(p, pfs_basename(proc->exe_path));
    *p++ = '\n';

    p = pfs_strcpy(p, "State:\t");
    switch (proc->task.state) {
    case TASK_RUNNING: p = pfs_strcpy(p, "R (running)"); break;
    case TASK_BLOCKED: p = pfs_strcpy(p, "S (sleeping)"); break;
    case TASK_ZOMBIE:  p = pfs_strcpy(p, "Z (zombie)"); break;
    case TASK_STOPPED: p = pfs_strcpy(p, "T (stopped)"); break;
    default:           p = pfs_strcpy(p, "? (unknown)"); break;
    }
    *p++ = '\n';

    p = pfs_strcpy(p, "Tgid:\t");
    p = pfs_u64_dec(p, proc->tgid);
    *p++ = '\n';

    p = pfs_strcpy(p, "Pid:\t");
    p = pfs_u64_dec(p, proc->pid);
    *p++ = '\n';

    p = pfs_strcpy(p, "PPid:\t");
    p = pfs_u64_dec(p, proc->ppid);
    *p++ = '\n';

    p = pfs_strcpy(p, "Uid:\t");
    p = pfs_u64_dec(p, proc->uid);
    *p++ = '\n';

    p = pfs_strcpy(p, "Gid:\t");
    p = pfs_u64_dec(p, proc->gid);
    *p++ = '\n';

    /* VmSize: sum of VMA lengths in kB */
    p = pfs_strcpy(p, "VmSize:\t");
    {
        uint64_t total = 0;
        uint32_t i;
        if (proc->vma_table) {
            for (i = 0; i < proc->vma_count; i++)
                total += proc->vma_table[i].len;
        }
        p = pfs_u64_dec(p, total / 1024);
    }
    p = pfs_strcpy(p, " kB\n");

    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_stat(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    (void)bufsz;

    /* pid (comm) state ppid pgid sid tty_nr tpgid ... */
    p = pfs_u64_dec(p, proc->pid);
    *p++ = ' ';
    *p++ = '(';
    p = pfs_strcpy(p, pfs_basename(proc->exe_path));
    *p++ = ')';
    *p++ = ' ';

    switch (proc->task.state) {
    case TASK_RUNNING: *p++ = 'R'; break;
    case TASK_BLOCKED: *p++ = 'S'; break;
    case TASK_ZOMBIE:  *p++ = 'Z'; break;
    case TASK_STOPPED: *p++ = 'T'; break;
    default:           *p++ = '?'; break;
    }
    *p++ = ' ';

    p = pfs_u64_dec(p, proc->ppid);
    *p++ = ' ';
    p = pfs_u64_dec(p, proc->pgid);
    *p++ = ' ';
    /* sid=0 tty_nr=0 tpgid=0 flags=0 ... pad with zeros for remaining fields */
    p = pfs_strcpy(p, "0 0 ");
    p = pfs_u64_dec(p, proc->tgid);
    p = pfs_strcpy(p, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");

    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_exe(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    (void)bufsz;
    p = pfs_strcpy(p, proc->exe_path);
    *p++ = '\n';
    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_cmdline(char *buf, uint32_t bufsz, aegis_process_t *proc)
{
    char *p = buf;
    (void)bufsz;
    p = pfs_strcpy(p, proc->exe_path);
    *p++ = '\0'; /* NUL-terminated argv[0] */
    return (uint32_t)(p - buf);
}

static uint32_t
gen_meminfo(char *buf, uint32_t bufsz)
{
    char *p = buf;
    (void)bufsz;
    uint64_t total_kb = pmm_total_pages() * 4;
    uint64_t free_kb  = pmm_free_pages()  * 4;

    p = pfs_strcpy(p, "MemTotal:       ");
    p = pfs_u64_dec(p, total_kb);
    p = pfs_strcpy(p, " kB\n");

    p = pfs_strcpy(p, "MemFree:        ");
    p = pfs_u64_dec(p, free_kb);
    p = pfs_strcpy(p, " kB\n");

    p = pfs_strcpy(p, "MemAvailable:   ");
    p = pfs_u64_dec(p, free_kb);
    p = pfs_strcpy(p, " kB\n");

    *p = '\0';
    return (uint32_t)(p - buf);
}

static uint32_t
gen_version(char *buf, uint32_t bufsz)
{
    char *p = buf;
    (void)bufsz;
    p = pfs_strcpy(p, "Aegis 0.31.0\n");
    *p = '\0';
    return (uint32_t)(p - buf);
}

/* ── capability check ──────────────────────────────────────────────────── */

static int
procfs_check_access(uint32_t target_pid)
{
    aegis_task_t *cur = sched_current();
    if (!cur || !cur->is_user)
        return -1;
    aegis_process_t *caller = (aegis_process_t *)cur;
    if (target_pid == caller->pid)
        return 0; /* self always OK */
    return cap_check(caller->caps, CAP_TABLE_SIZE,
                     CAP_KIND_PROC_READ, CAP_RIGHTS_READ);
}

/* ── VFS ops for generated-content files ───────────────────────────────── */

static int
procfs_file_read(void *priv, void *buf, uint64_t off, uint64_t len)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)priv;
    if (off >= fp->len)
        return 0;
    uint64_t avail = fp->len - off;
    if (len > avail)
        len = avail;
    __builtin_memcpy(buf, fp->buf + off, (uint32_t)len);
    return (int)len;
}

static void
procfs_file_close(void *priv)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)priv;
    if (fp->buf)
        kva_free_pages(fp->buf, 1);
    kva_free_pages(fp, 1);
}

static int
procfs_file_stat(void *priv, k_stat_t *st)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 5;
    st->st_ino   = 1;
    st->st_nlink = 1;
    st->st_mode  = S_IFREG | 0444;
    st->st_size  = (int64_t)fp->len;
    return 0;
}

static const vfs_ops_t s_procfs_file_ops = {
    .read    = procfs_file_read,
    .write   = 0,
    .close   = procfs_file_close,
    .readdir = 0,
    .dup     = 0,
    .stat    = procfs_file_stat,
};

/* ── VFS ops for directory listings ────────────────────────────────────── */

static int
procfs_dir_readdir(void *priv, uint64_t index, char *name_out, uint8_t *type_out)
{
    procfs_dir_priv_t *dp = (procfs_dir_priv_t *)priv;

    if (dp->is_fd) {
        /* /proc/[pid]/fd/ — enumerate open fds */
        aegis_process_t *proc = proc_find_by_pid(dp->pid);
        if (!proc || !proc->fd_table)
            return -1;
        uint64_t found = 0;
        uint32_t i;
        for (i = 0; i < PROC_MAX_FDS; i++) {
            if (!proc->fd_table->fds[i].ops)
                continue;
            if (found == index) {
                /* write decimal fd number into name_out */
                char *p = pfs_u64_dec(name_out, i);
                *p = '\0';
                *type_out = 8; /* DT_REG */
                return 0;
            }
            found++;
        }
        return -1;
    }

    if (dp->pid != 0) {
        /* /proc/[pid]/ — fixed entries */
        static const char *entries[] = {
            "maps", "status", "stat", "exe", "cmdline", "fd"
        };
        static const uint8_t types[] = {
            8, 8, 8, 8, 8, 4  /* DT_REG=8, DT_DIR=4 */
        };
        if (index < 6) {
            char *p = pfs_strcpy(name_out, entries[index]);
            *p = '\0';
            *type_out = types[index];
            return 0;
        }
        return -1;
    }

    /* /proc/ root — self, meminfo, version, then live PIDs */
    if (index == 0) {
        pfs_strcpy(name_out, "self");
        name_out[4] = '\0';
        *type_out = 4; /* DT_DIR */
        return 0;
    }
    if (index == 1) {
        pfs_strcpy(name_out, "meminfo");
        name_out[7] = '\0';
        *type_out = 8; /* DT_REG */
        return 0;
    }
    if (index == 2) {
        pfs_strcpy(name_out, "version");
        name_out[7] = '\0';
        *type_out = 8; /* DT_REG */
        return 0;
    }

    /* Enumerate user processes from circular task list */
    uint64_t pidx = index - 3;
    aegis_task_t *cur = sched_current();
    if (!cur)
        return -1;
    aegis_task_t *t = cur;
    uint64_t found = 0;
    do {
        if (t->is_user) {
            if (found == pidx) {
                aegis_process_t *proc = (aegis_process_t *)t;
                char *p = pfs_u64_dec(name_out, proc->pid);
                *p = '\0';
                *type_out = 4; /* DT_DIR */
                return 0;
            }
            found++;
        }
        t = t->next;
    } while (t != cur);

    return -1;
}

static void
procfs_dir_close(void *priv)
{
    kva_free_pages(priv, 1);
}

static int
procfs_dir_stat(void *priv, k_stat_t *st)
{
    (void)priv;
    __builtin_memset(st, 0, sizeof(*st));
    st->st_dev   = 5;
    st->st_ino   = 1;
    st->st_nlink = 2;
    st->st_mode  = S_IFDIR | 0555;
    return 0;
}

static const vfs_ops_t s_procfs_dir_ops = {
    .read    = 0,
    .write   = 0,
    .close   = procfs_dir_close,
    .readdir = procfs_dir_readdir,
    .dup     = 0,
    .stat    = procfs_dir_stat,
};

/* ── path helpers ──────────────────────────────────────────────────────── */

/* Parse a decimal PID from path; advance *pp past the digits.
 * Returns 0 if no digits found. */
static uint32_t
pfs_parse_pid(const char **pp)
{
    const char *s = *pp;
    uint32_t pid = 0;
    while (*s >= '0' && *s <= '9') {
        pid = pid * 10 + (uint32_t)(*s - '0');
        s++;
    }
    *pp = s;
    return pid;
}

/* Open a generated-content file into *out.
 * Allocates a kva page for the buffer, calls the generator, sets up the fd. */
static int
procfs_open_file(uint32_t (*gen)(char *, uint32_t, aegis_process_t *),
                 aegis_process_t *proc, vfs_file_t *out)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)kva_alloc_pages(1);
    char *buf = (char *)kva_alloc_pages(1);
    if (!fp || !buf)
        return -12; /* ENOMEM */
    fp->buf = buf;
    fp->len = gen(buf, 4096, proc);
    fp->_pad = 0;

    out->ops    = &s_procfs_file_ops;
    out->priv   = (void *)fp;
    out->offset = 0;
    out->size   = (uint64_t)fp->len;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

/* Open a global file (meminfo, version) — generator takes no proc arg. */
static int
procfs_open_global(uint32_t (*gen)(char *, uint32_t), vfs_file_t *out)
{
    procfs_file_priv_t *fp = (procfs_file_priv_t *)kva_alloc_pages(1);
    char *buf = (char *)kva_alloc_pages(1);
    if (!fp || !buf)
        return -12;
    fp->buf = buf;
    fp->len = gen(buf, 4096);
    fp->_pad = 0;

    out->ops    = &s_procfs_file_ops;
    out->priv   = (void *)fp;
    out->offset = 0;
    out->size   = (uint64_t)fp->len;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

/* Open a directory fd with a procfs_dir_priv_t. */
static int
procfs_open_dir(uint32_t pid, uint8_t is_fd, vfs_file_t *out)
{
    procfs_dir_priv_t *dp = (procfs_dir_priv_t *)kva_alloc_pages(1);
    if (!dp)
        return -12;
    dp->pid   = pid;
    dp->is_fd = is_fd;
    dp->_pad[0] = 0;
    dp->_pad[1] = 0;
    dp->_pad[2] = 0;

    out->ops    = &s_procfs_dir_ops;
    out->priv   = (void *)dp;
    out->offset = 0;
    out->size   = 0;
    out->flags  = 0;
    out->_pad   = 0;
    return 0;
}

/* Dispatch a per-pid subpath. path points AFTER the pid digits (e.g. "/maps" or ""). */
static int
procfs_open_pid(uint32_t pid, const char *path, vfs_file_t *out)
{
    int rc = procfs_check_access(pid);
    if (rc != 0)
        return -(int)ENOCAP;

    aegis_process_t *proc = proc_find_by_pid(pid);
    if (!proc)
        return -2; /* ENOENT */

    /* /proc/<pid> or /proc/<pid>/ → directory */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return procfs_open_dir(pid, 0, out);

    /* Skip leading slash */
    if (path[0] == '/')
        path++;

    if (pfs_streq(path, "maps"))
        return procfs_open_file(gen_maps, proc, out);
    if (pfs_streq(path, "status"))
        return procfs_open_file(gen_status, proc, out);
    if (pfs_streq(path, "stat"))
        return procfs_open_file(gen_stat, proc, out);
    if (pfs_streq(path, "exe"))
        return procfs_open_file(gen_exe, proc, out);
    if (pfs_streq(path, "cmdline"))
        return procfs_open_file(gen_cmdline, proc, out);
    if (pfs_streq(path, "fd") || pfs_streq(path, "fd/"))
        return procfs_open_dir(pid, 1, out);

    return -2; /* ENOENT */
}

/* ── public API ────────────────────────────────────────────────────────── */

void
procfs_init(void)
{
    /* No-op — procfs is a VFS backend, not a hardware subsystem. */
}

/*
 * procfs_open — open a /proc path.
 * path is relative to /proc/ (prefix stripped by vfs_open).
 * e.g. "self/maps", "meminfo", "1/status", "" (root dir).
 */
int
procfs_open(const char *path, int flags, vfs_file_t *out)
{
    (void)flags;

    /* Root /proc/ directory */
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return procfs_open_dir(0, 0, out);

    /* Skip leading slash if present */
    if (path[0] == '/')
        path++;

    /* Global files */
    if (pfs_streq(path, "meminfo"))
        return procfs_open_global(gen_meminfo, out);
    if (pfs_streq(path, "version"))
        return procfs_open_global(gen_version, out);

    /* /proc/self/... → resolve to current pid */
    if (path[0] == 's' && path[1] == 'e' && path[2] == 'l' && path[3] == 'f') {
        aegis_task_t *cur = sched_current();
        if (!cur || !cur->is_user)
            return -2;
        aegis_process_t *caller = (aegis_process_t *)cur;
        return procfs_open_pid(caller->pid, path + 4, out);
    }

    /* /proc/<pid>/... */
    if (path[0] >= '0' && path[0] <= '9') {
        const char *p = path;
        uint32_t pid = pfs_parse_pid(&p);
        if (pid == 0)
            return -2;
        return procfs_open_pid(pid, p, out);
    }

    return -2; /* ENOENT */
}

/*
 * procfs_stat — stat a /proc path.
 * path is the full path including "/proc" prefix.
 */
int
procfs_stat(const char *path, k_stat_t *out)
{
    if (!path || !out)
        return -2;

    __builtin_memset(out, 0, sizeof(*out));

    /* /proc itself */
    if (pfs_streq(path, "/proc") || pfs_streq(path, "/proc/")) {
        out->st_dev   = 5;
        out->st_ino   = 1;
        out->st_nlink = 2;
        out->st_mode  = S_IFDIR | 0555;
        return 0;
    }

    /* Strip /proc/ prefix */
    const char *rel = path + 5; /* past "/proc" */
    if (*rel == '/')
        rel++;

    /* Global files */
    if (pfs_streq(rel, "meminfo") || pfs_streq(rel, "version")) {
        out->st_dev   = 5;
        out->st_ino   = 1;
        out->st_nlink = 1;
        out->st_mode  = S_IFREG | 0444;
        return 0;
    }

    /* /proc/self → directory */
    if (pfs_streq(rel, "self") || pfs_streq(rel, "self/")) {
        out->st_dev   = 5;
        out->st_ino   = 1;
        out->st_nlink = 2;
        out->st_mode  = S_IFDIR | 0555;
        return 0;
    }

    /* /proc/self/<file> */
    if (rel[0] == 's' && rel[1] == 'e' && rel[2] == 'l' && rel[3] == 'f' && rel[4] == '/') {
        const char *sub = rel + 5;
        if (pfs_streq(sub, "fd") || pfs_streq(sub, "fd/")) {
            out->st_dev   = 5;
            out->st_ino   = 1;
            out->st_nlink = 2;
            out->st_mode  = S_IFDIR | 0555;
            return 0;
        }
        if (pfs_streq(sub, "maps") || pfs_streq(sub, "status") ||
            pfs_streq(sub, "stat") || pfs_streq(sub, "exe") ||
            pfs_streq(sub, "cmdline")) {
            out->st_dev   = 5;
            out->st_ino   = 1;
            out->st_nlink = 1;
            out->st_mode  = S_IFREG | 0444;
            return 0;
        }
        return -2;
    }

    /* /proc/<pid> or /proc/<pid>/<file> */
    if (rel[0] >= '0' && rel[0] <= '9') {
        const char *p = rel;
        uint32_t pid = pfs_parse_pid(&p);
        if (pid == 0)
            return -2;

        /* Check process exists */
        aegis_process_t *proc = proc_find_by_pid(pid);
        if (!proc)
            return -2;

        /* /proc/<pid> → directory */
        if (*p == '\0' || (*p == '/' && *(p + 1) == '\0')) {
            out->st_dev   = 5;
            out->st_ino   = 1;
            out->st_nlink = 2;
            out->st_mode  = S_IFDIR | 0555;
            return 0;
        }

        if (*p == '/')
            p++;

        if (pfs_streq(p, "fd") || pfs_streq(p, "fd/")) {
            out->st_dev   = 5;
            out->st_ino   = 1;
            out->st_nlink = 2;
            out->st_mode  = S_IFDIR | 0555;
            return 0;
        }
        if (pfs_streq(p, "maps") || pfs_streq(p, "status") ||
            pfs_streq(p, "stat") || pfs_streq(p, "exe") ||
            pfs_streq(p, "cmdline")) {
            out->st_dev   = 5;
            out->st_ino   = 1;
            out->st_nlink = 1;
            out->st_mode  = S_IFREG | 0444;
            return 0;
        }
        return -2;
    }

    return -2;
}
