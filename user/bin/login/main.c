/* login — text-mode login binary for Aegis.
 *
 * Authenticates via libauth.a, then execve's the user's shell.
 * Capabilities: AUTH and SETUID from kernel policy table (service tier).
 * After successful auth, calls auth_elevate_session() so the spawned
 * shell gets admin-tier caps from the policy table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "auth.h"

extern char **environ;

#define MAX_ATTEMPTS 3
#define FAIL_DELAY   3

/* Read a line from fd, stripping trailing newline. */
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

int
main(void)
{
    char username[64];
    char password[128];
    char home[256];
    char shell[256];
    int  uid = 0, gid = 0;

    /* AUTH + SETUID caps come from kernel policy table (service tier).
     * No runtime cap request needed — login is listed in caps.d/login. */

    /* Display pre-auth banner */
    {
        int bfd = open("/etc/banner", O_RDONLY);
        if (bfd >= 0) {
            char bbuf[512];
            int br;
            while ((br = (int)read(bfd, bbuf, sizeof(bbuf))) > 0)
                write(1, bbuf, (size_t)br);
            close(bfd);
        }
    }

    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        write(1, "\nlogin: ", 8);
        if (readline(0, username, (int)sizeof(username)) < 0) continue;
        if (username[0] == '\0') { attempt--; continue; }

        /* Disable echo for password input */
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
                if (c == '\x7f' || c == '\b') {
                    if (pi > 0) { pi--; write(1, "\b \b", 3); }
                    continue;
                }
                password[pi++] = c;
                write(1, "*", 1);
            }
            password[pi] = '\0';
        }
        write(1, "\n", 1);
        tcsetattr(0, TCSANOW, &t);

        /* Authenticate */
        if (auth_check(username, password, &uid, &gid,
                       home, (int)sizeof(home), shell, (int)sizeof(shell)) != 0) {
            sleep(FAIL_DELAY);
            write(1, "Login incorrect\n", 16);
            continue;
        }

        /* Success — elevate session, set identity, and launch shell */
        auth_elevate_session();
        write(1, "\033[2J\033[H", 7);
        auth_set_identity(uid, gid);
        chdir(home);

        setenv("HOME",    home,     1);
        setenv("USER",    username, 1);
        setenv("LOGNAME", username, 1);
        setenv("SHELL",   shell,    1);
        setenv("PATH",    "/bin",   1);

        /* Build login shell name with leading '-' */
        char login_shell[64];
        const char *base = strrchr(shell, '/');
        const char *name = base ? base + 1 : shell;
        login_shell[0] = '-';
        int nlen = (int)strlen(name);
        if (nlen > 62) nlen = 62;
        memcpy(login_shell + 1, name, (size_t)nlen);
        login_shell[1 + nlen] = '\0';

        auth_grant_shell_caps();

        char *argv[] = { login_shell, NULL };
        execve(shell, argv, environ);
        /* Fallback */
        char *fb_argv[] = { "-sh", NULL };
        execve("/bin/sh", fb_argv, NULL);
        write(2, "login: execve failed\n", 21);
        return 1;
    }

    write(1, "Maximum login attempts exceeded.\n", 33);
    return 1;
}
