/* login — capability-delegating login binary for Aegis.
 *
 * Capabilities held at start (granted by proc_spawn):
 *   CAP_KIND_VFS_OPEN, VFS_READ, VFS_WRITE, AUTH, CAP_GRANT, SETUID
 *
 * After execve(shell):
 *   shell holds only baseline caps (execve resets to VFS_OPEN/READ/WRITE).
 *   CAP_KIND_AUTH and CAP_KIND_SETUID do not survive exec.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/syscall.h>

#define MAX_ATTEMPTS 3
#define FAIL_DELAY   3

/* Read a line from fd into buf (max len-1 bytes), stripping trailing \n.
 * Returns number of bytes read, or -1 on EOF/error. */
static int
readline(int fd, char *buf, int len)
{
    int i = 0;
    char c;
    while (i < len - 1) {
        int n = (int)read(fd, &c, 1);
        if (n <= 0) return (i > 0) ? i : -1;
        if (c == '\n' || c == '\r') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* Look up username in /etc/passwd. Returns 0 on success, -1 on failure.
 * Fills uid, gid, home, shell from the matching line. */
static int
lookup_passwd(const char *username, int *uid, int *gid,
              char *home, int homelen, char *shell, int shelllen)
{
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Format: name:x:uid:gid:gecos:home:shell */
        char *fields[7];
        char *p = line, *q;
        int f = 0;
        while (f < 7 && (q = strchr(p, ':'))) {
            *q = '\0';
            fields[f++] = p;
            p = q + 1;
        }
        /* last field (shell) ends with \n */
        if (f == 6) {
            q = strchr(p, '\n');
            if (q) *q = '\0';
            fields[f++] = p;
        }
        if (f < 7) continue;
        if (strcmp(fields[0], username) != 0) continue;
        *uid = atoi(fields[2]);
        *gid = atoi(fields[3]);
        strncpy(home,  fields[5], (size_t)homelen  - 1); home[homelen-1]  = '\0';
        strncpy(shell, fields[6], (size_t)shelllen - 1); shell[shelllen-1] = '\0';
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

/* Look up shadow hash for username. Returns 0 on success, -1 on failure.
 * Fills hash buffer. Requires CAP_KIND_AUTH to open /etc/shadow. */
static int
lookup_shadow(const char *username, char *hash, int hashlen)
{
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0) {
        write(2, "login: cannot open /etc/shadow\n", 31);
        return -1;
    }
    char filebuf[2048];
    int  total = 0, n;
    while ((n = (int)read(fd, filebuf + total,
                          (size_t)((int)sizeof(filebuf) - total - 1))) > 0)
        total += n;
    close(fd);
    filebuf[total] = '\0';

    char *line = filebuf;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            if (strcmp(line, username) == 0) {
                char *h = colon + 1;
                char *hend = strchr(h, ':');
                if (hend) *hend = '\0';
                strncpy(hash, h, (size_t)hashlen - 1);
                hash[hashlen-1] = '\0';
                return 0;
            }
            *colon = ':'; /* restore for next iteration */
        }
        if (!end) break;
        line = end + 1;
    }
    return -1;
}

int
main(void)
{
    char username[64];
    char password[128];
    char hash[256];
    char home[256];
    char shell[256];
    int  uid = 0, gid = 0;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        /* Prompt username */
        write(1, "\nlogin: ", 8);
        if (readline(0, username, (int)sizeof(username)) < 0) continue;
        if (username[0] == '\0') { attempt--; continue; }

        /* Look up passwd */
        if (lookup_passwd(username, &uid, &gid, home, (int)sizeof(home),
                          shell, (int)sizeof(shell)) != 0) {
            sleep(FAIL_DELAY);
            write(1, "Login incorrect\n", 16);
            continue;
        }

        /* Look up shadow hash */
        if (lookup_shadow(username, hash, (int)sizeof(hash)) != 0) {
            sleep(FAIL_DELAY);
            write(1, "Login incorrect\n", 16);
            continue;
        }

        /* Disable echo and canonical mode for password — read char by char */
        struct termios t;
        tcgetattr(0, &t);
        struct termios t_raw = t;
        t_raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
        tcsetattr(0, TCSANOW, &t_raw);

        write(1, "password: ", 10);
        {
            int pi = 0;
            char c;
            while (pi < (int)sizeof(password) - 1) {
                int n = (int)read(0, &c, 1);
                if (n <= 0) break;
                if (c == '\n' || c == '\r') break;
                password[pi++] = c;
                write(1, "*", 1);
            }
            password[pi] = '\0';
        }
        write(1, "\n", 1);
        int plen = (int)strlen(password);

        /* Restore terminal */
        tcsetattr(0, TCSANOW, &t);

        if (plen < 0) continue;

        /* Verify password */
        char *computed = crypt(password, hash);
        if (!computed || strcmp(computed, hash) != 0) {
            sleep(FAIL_DELAY);
            write(1, "Login incorrect\n", 16);
            continue;
        }

        /* Authentication succeeded */
        syscall(105, (long)uid);  /* sys_setuid */
        syscall(106, (long)gid);  /* sys_setgid */

        chdir(home);

        setenv("HOME",    home,     1);
        setenv("USER",    username, 1);
        setenv("LOGNAME", username, 1);
        setenv("SHELL",   shell,    1);
        setenv("PATH",    "/bin",   1);

        /* Build login shell name: "-oksh" → login shell sources /etc/profile.
         * Truncation to 62 chars is intentional — no real shell basename
         * exceeds that length. */
        char login_shell[64];
        const char *base = strrchr(shell, '/');
        const char *name = base ? base + 1 : shell;
        login_shell[0] = '-';
        /* Intentional truncation: shell basenames are always short. */
        int nlen = (int)strlen(name);
        if (nlen > 62) nlen = 62;
        memcpy(login_shell + 1, name, (size_t)nlen);
        login_shell[1 + nlen] = '\0';

        char *argv[] = { login_shell, NULL };
        execve(shell, argv, NULL);
        /* execve failed */
        write(2, "login: execve failed\n", 21);
        return 1;
    }

    write(1, "Maximum login attempts exceeded.\n", 33);
    return 1;
}
