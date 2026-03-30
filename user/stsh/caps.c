#include "stsh.h"

/* Human-readable capability names (must match kernel cap kinds) */
static const char *s_cap_names[] = {
    "VFS_OPEN",        /* 0 */
    "VFS_READ",        /* 1 */
    "VFS_WRITE",       /* 2 */
    "NET_BIND",        /* 3 */
    "NET_CONNECT",     /* 4 */
    "NET_RAW",         /* 5 */
    "PROC_SIGNAL",     /* 6 */
    "PROC_INFO",       /* 7 */
    "FB_ACCESS",       /* 8 */
    "DISK_ADMIN",      /* 9 */
    "THREAD_CREATE",   /* 10 */
    "PROC_READ",       /* 11 */
    "AUTH",            /* 12 */
    "CAP_DELEGATE",    /* 13 */
    "CAP_QUERY",       /* 14 */
};
#define CAP_NAME_COUNT (int)(sizeof(s_cap_names) / sizeof(s_cap_names[0]))

static int s_has_delegate;
static int s_has_query;

static long
sys_cap_query(long pid)
{
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_CAP_QUERY), "D"(pid)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long
sys_spawn_caps(const char *path, char **argv, char **envp,
               int stdio_fd, const cap_slot_t *mask, int mask_count)
{
    long ret;
    register long r8  __asm__("r8")  = (long)mask;
    register long r9  __asm__("r9")  = (long)mask_count;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_SPAWN), "D"((long)path), "S"((long)argv),
          "d"((long)envp), "r"(r8), "r"(r9)
        /* Note: rcx used for stdio_fd via register constraint not
         * available in x86_64 syscall ABI. sys_spawn uses r10 for
         * stdio_fd per Aegis convention. */
        : "rcx", "r11", "memory"
    );
    return ret;
}

/*
 * format_rights — format rights bitmask as string (rwx).
 */
static void
format_rights(unsigned int rights, char *out)
{
    int i = 0;
    if (rights & 1) out[i++] = 'r';
    if (rights & 2) out[i++] = 'w';
    if (rights & 4) out[i++] = 'x';
    if (i == 0) out[i++] = '-';
    out[i] = '\0';
}

/*
 * caps_init — cache whether we hold CAP_DELEGATE and CAP_QUERY.
 */
void
caps_init(void)
{
    long ret = sys_cap_query(0);
    if (ret < 0) {
        /* No CAP_QUERY — can't enumerate caps */
        s_has_delegate = 0;
        s_has_query    = 0;
        return;
    }

    s_has_query = 1;

    /* sys_cap_query returns a bitmask of capability kinds held.
     * Bit N set = cap kind N is present. */
    s_has_delegate = (ret & (1L << 13)) ? 1 : 0;
}

/*
 * has_cap_delegate — returns 1 if this process holds CAP_DELEGATE.
 */
int
has_cap_delegate(void)
{
    return s_has_delegate;
}

/*
 * cap_name_to_kind — look up a capability name and return its kind number.
 * Returns -1 if not found. Case-insensitive comparison.
 */
static int
cap_name_to_kind(const char *name)
{
    for (int i = 0; i < CAP_NAME_COUNT; i++) {
        const char *a = name;
        const char *b = s_cap_names[i];
        int match = 1;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) { match = 0; break; }
            a++; b++;
        }
        if (match && !*a && !*b)
            return i;
    }
    return -1;
}

/*
 * caps_builtin — display capabilities.
 * No args = own caps (pid=0). With PID arg = target's caps.
 */
int
caps_builtin(int argc, char **argv)
{
    if (!s_has_query) {
        fprintf(stderr, "caps: no CAP_QUERY capability\n");
        return 1;
    }

    long pid = 0;
    if (argc >= 2)
        pid = atol(argv[1]);

    long ret = sys_cap_query(pid);
    if (ret < 0) {
        fprintf(stderr, "caps: query failed (%ld)\n", ret);
        return 1;
    }

    /* ret is a bitmask of capability kinds */
    int first = 1;
    for (int i = 0; i < CAP_NAME_COUNT; i++) {
        if (ret & (1L << i)) {
            if (!first)
                printf(" ");
            /* For display, show the rights as the kind implies.
             * READ kinds get 'r', WRITE kinds get 'w', etc.
             * Since sys_cap_query returns a kind bitmask (not rights),
             * we show all granted rights. */
            char rights_str[4];
            /* Default: show 'r' for most, 'w' for write, 'rwx' for admin */
            unsigned int display_rights = 1; /* read */
            if (i == 2) display_rights = 2;  /* VFS_WRITE */
            if (i == 9) display_rights = 7;  /* DISK_ADMIN */
            if (i == 13) display_rights = 1; /* CAP_DELEGATE */
            if (i == 14) display_rights = 1; /* CAP_QUERY */
            format_rights(display_rights, rights_str);
            printf("%s(%s)", s_cap_names[i], rights_str);
            first = 0;
        }
    }
    if (!first)
        printf("\n");
    return 0;
}

/*
 * sandbox_builtin — run a command with restricted capabilities.
 * Usage: sandbox -allow CAP1,CAP2 -- command args...
 */
int
sandbox_builtin(int argc, char **argv, char **envp)
{
    if (!s_has_delegate) {
        fprintf(stderr, "sandbox: no CAP_DELEGATE capability\n");
        return 1;
    }

    /* Parse -allow CAP1,CAP2 -- command args... */
    cap_slot_t mask[CAP_TABLE_SIZE];
    int mask_count = 0;
    int cmd_start = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd_start = i + 1;
            break;
        }
        if (strcmp(argv[i], "-allow") == 0 && i + 1 < argc) {
            i++;
            /* Parse comma-separated cap names */
            char caplist[512];
            strncpy(caplist, argv[i], sizeof(caplist) - 1);
            caplist[sizeof(caplist) - 1] = '\0';

            char *tok = caplist;
            while (tok && *tok && mask_count < CAP_TABLE_SIZE) {
                char *comma = strchr(tok, ',');
                if (comma) *comma = '\0';

                int kind = cap_name_to_kind(tok);
                if (kind >= 0) {
                    mask[mask_count].kind   = (unsigned int)kind;
                    mask[mask_count].rights = 1 | 2 | 4; /* all rights */
                    mask_count++;
                } else {
                    fprintf(stderr, "sandbox: unknown capability: %s\n", tok);
                    return 1;
                }

                tok = comma ? comma + 1 : NULL;
            }
        }
    }

    if (cmd_start < 0 || cmd_start >= argc) {
        fprintf(stderr,
                "usage: sandbox -allow CAP1,CAP2 -- command [args...]\n");
        return 1;
    }

    /* Resolve command path */
    char path[256];
    if (argv[cmd_start][0] == '/') {
        strncpy(path, argv[cmd_start], sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        snprintf(path, sizeof(path), "/bin/%s", argv[cmd_start]);
    }

    /* Build argv for spawned process */
    char **spawn_argv = &argv[cmd_start];

    long pid = sys_spawn_caps(path, spawn_argv, envp, -1,
                              mask_count > 0 ? mask : NULL, mask_count);
    if (pid < 0) {
        fprintf(stderr, "sandbox: spawn failed (%ld)\n", pid);
        return 1;
    }

    /* Wait for the sandboxed child */
    int status;
    waitpid((int)pid, &status, 0);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}
