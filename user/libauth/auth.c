/* auth.c — shared authentication library for Aegis.
 *
 * Provides credential verification against /etc/passwd + /etc/shadow,
 * identity switching (setuid/setgid), and capability pre-registration
 * for the user's shell. Used by both /bin/login and /bin/bastion.
 */
#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>

/* ---- capd client ------------------------------------------------------ */

int
capd_request(unsigned int kind)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/capd.sock", sizeof(addr.sun_path) - 1);
    for (int retry = 0; retry < 3; retry++) {
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            goto connected;
        usleep(100000);
    }
    close(sock);
    return -1;
connected:;
    write(sock, &kind, sizeof(kind));
    int result = -1;
    read(sock, &result, sizeof(result));
    close(sock);
    return result >= 0 ? 0 : -1;
}

/* ---- passwd/shadow lookup --------------------------------------------- */

int
auth_lookup_passwd(const char *username, int *uid, int *gid,
                   char *home, int homelen, char *shell, int shelllen)
{
    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *fields[7];
        char *p = line, *q;
        int f = 0;
        while (f < 7 && (q = strchr(p, ':'))) {
            *q = '\0';
            fields[f++] = p;
            p = q + 1;
        }
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

int
auth_lookup_shadow(const char *username, char *hash, int hashlen)
{
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0) return -1;
    char filebuf[2048];
    int total = 0, n;
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
                hash[hashlen - 1] = '\0';
                return 0;
            }
            *colon = ':';
        }
        if (!end) break;
        line = end + 1;
    }
    return -1;
}

/* ---- verification ----------------------------------------------------- */

int
auth_verify(const char *password, const char *hash)
{
    char *computed = crypt(password, hash);
    if (!computed || strcmp(computed, hash) != 0)
        return -1;
    return 0;
}

int
auth_check(const char *username, const char *password,
           int *uid, int *gid,
           char *home, int homelen, char *shell, int shelllen)
{
    if (auth_lookup_passwd(username, uid, gid, home, homelen, shell, shelllen) != 0)
        return -1;
    char hash[256];
    if (auth_lookup_shadow(username, hash, (int)sizeof(hash)) != 0)
        return -1;
    return auth_verify(password, hash);
}

/* ---- identity + caps -------------------------------------------------- */

void
auth_set_identity(int uid, int gid)
{
    syscall(105, (long)uid);  /* sys_setuid */
    syscall(106, (long)gid);  /* sys_setgid */
}

void
auth_grant_shell_caps(void)
{
    syscall(361, 13L, 1L);  /* CAP_KIND_CAP_DELEGATE, CAP_RIGHTS_READ */
    syscall(361, 14L, 1L);  /* CAP_KIND_CAP_QUERY, CAP_RIGHTS_READ */
}

void
auth_request_caps(void)
{
    capd_request(4);   /* CAP_KIND_AUTH */
    capd_request(5);   /* CAP_KIND_CAP_GRANT */
    capd_request(13);  /* CAP_KIND_CAP_DELEGATE */
    capd_request(14);  /* CAP_KIND_CAP_QUERY */
    capd_request(6);   /* CAP_KIND_SETUID */
}
