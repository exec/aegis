/* user/capd/main.c — Capability broker daemon for Aegis.
 *
 * Listens on /run/capd.sock (AF_UNIX SOCK_STREAM).
 * Reads declarative policy files from /etc/aegis/capd.d/<basename>.policy.
 * Validates requests against policy using SO_PEERCRED + /proc/<pid>/exe.
 * Grants capabilities via sys_cap_grant (syscall 363).
 *
 * Binary protocol: client sends uint32_t kind, server responds int32_t result
 * (0=OK, negative=error code).
 *
 * capd itself holds all delegatable capabilities (via vigil exec_caps).
 * It is the single bootstrap root — the "cap root".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>

#define SYS_CAP_GRANT 363

#define MAX_POLICIES    16
#define MAX_CAPS_PER    16
#define SOCK_PATH       "/run/capd.sock"
#define POLICY_DIR      "/etc/aegis/capd.d"

/* ---- Cap name ↔ kind table ------------------------------------------- */

static const struct { const char *name; int kind; } cap_table[] = {
    { "VFS_OPEN",       1 },
    { "VFS_WRITE",      2 },
    { "VFS_READ",       3 },
    { "AUTH",           4 },
    { "CAP_GRANT",      5 },
    { "SETUID",         6 },
    { "NET_SOCKET",     7 },
    { "NET_ADMIN",      8 },
    { "THREAD_CREATE",  9 },
    { "PROC_READ",     10 },
    { "DISK_ADMIN",    11 },
    { "FB",            12 },
    { "CAP_DELEGATE",  13 },
    { "CAP_QUERY",     14 },
    { "IPC",           15 },
    { NULL, 0 }
};

static const char *
cap_kind_name(int kind)
{
    for (int i = 0; cap_table[i].name; i++)
        if (cap_table[i].kind == kind)
            return cap_table[i].name;
    return "?";
}

static int
cap_name_to_kind(const char *name)
{
    for (int i = 0; cap_table[i].name; i++)
        if (strcmp(cap_table[i].name, name) == 0)
            return cap_table[i].kind;
    return -1;
}

/* ---- Policy table ----------------------------------------------------- */

typedef struct {
    char basename[64];
    int  allowed[MAX_CAPS_PER];
    int  count;
} policy_t;

static policy_t s_policies[MAX_POLICIES];
static int      s_policy_count;

static int
policy_allows(const char *basename, int kind)
{
    for (int i = 0; i < s_policy_count; i++) {
        if (strcmp(s_policies[i].basename, basename) != 0)
            continue;
        for (int j = 0; j < s_policies[i].count; j++)
            if (s_policies[i].allowed[j] == kind)
                return 1;
        return 0;
    }
    return -1;
}

static int
load_policy_file(const char *path, const char *basename)
{
    if (s_policy_count >= MAX_POLICIES)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[256];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    if (strncmp(buf, "allow ", 6) != 0)
        return -1;

    policy_t *p = &s_policies[s_policy_count];
    strncpy(p->basename, basename, sizeof(p->basename) - 1);
    p->basename[sizeof(p->basename) - 1] = '\0';
    p->count = 0;

    char *tok = buf + 6;
    while (*tok && p->count < MAX_CAPS_PER) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!*tok) break;
        char *end = tok;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n') end++;
        char saved = *end;
        *end = '\0';
        int kind = cap_name_to_kind(tok);
        if (kind > 0)
            p->allowed[p->count++] = kind;
        if (!saved) break;
        tok = end + 1;
    }

    s_policy_count++;
    return 0;
}

static void
load_policies(void)
{
    int dfd = open(POLICY_DIR, O_RDONLY);
    if (dfd < 0) return;

    char dentbuf[1024];
    long nr;
    while ((nr = syscall(SYS_getdents64, dfd, dentbuf, sizeof(dentbuf))) > 0) {
        long off = 0;
        while (off < nr) {
            unsigned short reclen;
            memcpy(&reclen, dentbuf + off + 16, 2);
            char *name = dentbuf + off + 19;
            int namelen = (int)strlen(name);
            if (namelen > 7 && strcmp(name + namelen - 7, ".policy") == 0) {
                char basename[64];
                int blen = namelen - 7;
                if (blen > 63) blen = 63;
                memcpy(basename, name, (size_t)blen);
                basename[blen] = '\0';
                char path[256];
                snprintf(path, sizeof(path), "%s/%s", POLICY_DIR, name);
                load_policy_file(path, basename);
            }
            off += reclen;
        }
    }
    close(dfd);
    dprintf(2, "capd: loaded %d policies\n", s_policy_count);
}

/* ---- Identity lookup -------------------------------------------------- */

static int
get_exe_basename(int pid, char *out, int outlen)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[256];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    const char *base = buf;
    const char *slash = strrchr(buf, '/');
    if (slash) base = slash + 1;
    strncpy(out, base, (size_t)(outlen - 1));
    out[outlen - 1] = '\0';
    return 0;
}

/* ---- Exact-size read helper ------------------------------------------- */

/* Read exactly `len` bytes, looping on short reads.
 * Returns 0 on success, -1 on EOF/error. */
static int
read_exact(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* ---- Request handler -------------------------------------------------- */

static void
handle_client(int cfd)
{
    /* Get peer credentials */
    struct ucred { int pid; int uid; int gid; };
    struct ucred cred;
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(cfd, SOL_SOCKET, 17 /* SO_PEERCRED */, &cred, &cred_len) < 0)
        return;

    /* Look up client binary */
    char basename[64];
    if (get_exe_basename(cred.pid, basename, (int)sizeof(basename)) < 0) {
        int32_t res = -1;
        write(cfd, &res, sizeof(res));
        return;
    }

    /* Read request: uint32_t kind */
    uint32_t kind = 0;
    if (read_exact(cfd, &kind, sizeof(kind)) < 0) {
        dprintf(2, "capd: read failed for pid %d (%s)\n", cred.pid, basename);
        return;
    }
    dprintf(2, "capd: request kind=%u from pid %d (%s)\n", kind, cred.pid, basename);

    /* Validate cap kind */
    if (kind == 0 || kind >= 16) {
        int32_t res = -22; /* EINVAL */
        write(cfd, &res, sizeof(res));
        return;
    }

    /* Check policy */
    int allowed = policy_allows(basename, (int)kind);
    if (allowed <= 0) {
        int32_t res = -13; /* EACCES */
        write(cfd, &res, sizeof(res));
        dprintf(2, "capd: DENIED %s -> pid %d (%s)\n",
                cap_kind_name((int)kind), cred.pid, basename);
        return;
    }

    /* Grant via syscall — full rights (r|w|x = 7) */
    long rc = syscall(SYS_CAP_GRANT, (long)cred.pid, (long)kind, 7L);
    int32_t res = (int32_t)rc;
    write(cfd, &res, sizeof(res));

    if (rc >= 0)
        dprintf(2, "capd: GRANT %s -> pid %d (%s) OK\n",
                cap_kind_name((int)kind), cred.pid, basename);
    else
        dprintf(2, "capd: GRANT %s -> pid %d (%s) FAILED: %ld\n",
                cap_kind_name((int)kind), cred.pid, basename, rc);
}

/* ---- main ------------------------------------------------------------- */

int
main(void)
{
    load_policies();

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { dprintf(2, "capd: socket failed\n"); return 1; }

    unlink(SOCK_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dprintf(2, "capd: bind failed\n"); return 1;
    }
    if (listen(sfd, 8) < 0) {
        dprintf(2, "capd: listen failed\n"); return 1;
    }

    dprintf(2, "capd: listening on %s\n", SOCK_PATH);

    while (1) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;
        handle_client(cfd);
        close(cfd);
    }
    return 0;
}
