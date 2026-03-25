#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

#define VIGIL_MAX_SERVICES  16
#define VIGIL_CMD_PATH      "/run/vigil.cmd"
#define VIGIL_SERVICES_DIR  "/etc/vigil/services"
#define VIGIL_PID_PATH      "/run/vigil.pid"

typedef enum { POLICY_RESPAWN, POLICY_ONESHOT } policy_t;

typedef struct {
    char     name[64];
    char     run_cmd[256];
    policy_t policy;
    int      max_restarts;
    pid_t    pid;
    int      restarts;
    int      active;
} service_t;

static service_t s_svcs[VIGIL_MAX_SERVICES];
static int       s_nsvc = 0;
static volatile int s_got_usr1  = 0;
static volatile int s_got_term  = 0;

struct linux_dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[256];
};

static void
vigil_log(const char *msg)
{
    write(1, "vigil: ", 7);
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}

static void handle_usr1(int sig) { (void)sig; s_got_usr1 = 1; }
static void handle_term(int sig) { (void)sig; s_got_term = 1; }

static int
read_file(const char *path, char *buf, int bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, (size_t)(bufsz - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return n;
}

static void
load_service(const char *name)
{
    if (s_nsvc >= VIGIL_MAX_SERVICES) return;
    service_t *s = &s_svcs[s_nsvc];
    memset(s, 0, sizeof(*s));
    /* Use memcpy after bounding to avoid -Wstringop-truncation on strncpy */
    size_t nlen = strlen(name);
    if (nlen >= sizeof(s->name)) nlen = sizeof(s->name) - 1;
    memcpy(s->name, name, nlen);
    s->name[nlen] = '\0';

    char path[320];

    snprintf(path, sizeof(path), "%s/%s/run", VIGIL_SERVICES_DIR, name);
    if (read_file(path, s->run_cmd, sizeof(s->run_cmd)) <= 0) return;

    char pol[128] = "respawn";
    snprintf(path, sizeof(path), "%s/%s/policy", VIGIL_SERVICES_DIR, name);
    read_file(path, pol, sizeof(pol));
    s->policy = (strncmp(pol, "oneshot", 7) == 0) ? POLICY_ONESHOT : POLICY_RESPAWN;
    s->max_restarts = 5;
    char *p = strstr(pol, "max_restarts=");
    if (p) s->max_restarts = atoi(p + 13);

    s->pid      = -1;
    s->restarts = 0;
    s->active   = 1;
    s_nsvc++;
    vigil_log(name);
}

static void
start_service(service_t *s)
{
    if (s->pid > 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = { "/bin/sh", "-c", s->run_cmd, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    if (pid > 0) s->pid = pid;
}

static void
shutdown_all(void)
{
    int i;
    vigil_log("shutting down");
    for (i = 0; i < s_nsvc; i++)
        if (s_svcs[i].pid > 0) kill(s_svcs[i].pid, SIGTERM);

    struct timespec ts = { 1, 0 };
    int waited = 0;
    while (waited < 5) {
        int any = 0;
        for (i = 0; i < s_nsvc; i++)
            if (s_svcs[i].pid > 0) { any = 1; break; }
        if (!any) break;
        nanosleep(&ts, NULL);
        int status;
        pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0)
            for (i = 0; i < s_nsvc; i++)
                if (s_svcs[i].pid == p) s_svcs[i].pid = -1;
        waited++;
    }
    for (i = 0; i < s_nsvc; i++)
        if (s_svcs[i].pid > 0) kill(s_svcs[i].pid, SIGKILL);
}

static void
scan_services(void)
{
    int fd = open(VIGIL_SERVICES_DIR, O_RDONLY);
    if (fd < 0) { vigil_log("cannot open " VIGIL_SERVICES_DIR); return; }

    char buf[4096];
    long n;
    while ((n = syscall(217, fd, buf, sizeof(buf))) > 0) {
        long pos = 0;
        while (pos < n) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + pos);
            if (de->d_name[0] != '.' && de->d_type == 4)
                load_service(de->d_name);
            pos += de->d_reclen;
        }
    }
    close(fd);
}

static void
process_cmd(void)
{
    char cmd[192];
    if (read_file(VIGIL_CMD_PATH, cmd, sizeof(cmd)) <= 0) return;
    unlink(VIGIL_CMD_PATH);

    int i;
    if (strncmp(cmd, "status", 6) == 0) {
        char msg[160];
        for (i = 0; i < s_nsvc; i++) {
            /* name is at most 63 chars; format the fixed parts separately */
            write(1, "vigil: service ", 15);
            write(1, s_svcs[i].name, strlen(s_svcs[i].name));
            snprintf(msg, sizeof(msg), " pid=%d restarts=%d",
                     (int)s_svcs[i].pid, s_svcs[i].restarts);
            write(1, msg, strlen(msg));
            write(1, "\n", 1);
        }
        return;
    }
    char *sp = strchr(cmd, ' ');
    if (!sp) return;
    *sp = '\0';
    const char *svc_name = sp + 1;
    for (i = 0; i < s_nsvc; i++) {
        if (strcmp(s_svcs[i].name, svc_name) != 0) continue;
        if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "restart") == 0) {
            if (s_svcs[i].pid > 0) kill(s_svcs[i].pid, SIGTERM);
            s_svcs[i].active = (strcmp(cmd, "restart") == 0) ? 1 : 0;
        } else if (strcmp(cmd, "start") == 0) {
            s_svcs[i].active = 1;
            start_service(&s_svcs[i]);
        }
    }
}

int
main(void)
{
    vigil_log("starting");

    /* write PID file */
    {
        char pidbuf[32];
        int n = snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
        int fd = open(VIGIL_PID_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, pidbuf, (size_t)n); close(fd); }
    }

    signal(SIGUSR1, handle_usr1);
    signal(SIGTERM, handle_term);
    signal(SIGINT,  handle_term);
    signal(SIGCHLD, SIG_DFL);

    scan_services();

    /* Fallback: if no services were loaded (e.g. /etc/vigil/services doesn't
     * exist on this disk), register a built-in getty that runs login. */
    if (s_nsvc == 0) {
        vigil_log("no services, starting getty");
        service_t *s = &s_svcs[s_nsvc++];
        memset(s, 0, sizeof(*s));
        memcpy(s->name, "getty", 5);
        memcpy(s->run_cmd, "exec /bin/login", 15);
        s->policy       = POLICY_RESPAWN;
        s->max_restarts = 5;
        s->pid          = -1;
        s->active       = 1;
    }

    int i;
    for (i = 0; i < s_nsvc; i++)
        start_service(&s_svcs[i]);

    while (!s_got_term) {
        if (s_got_usr1) {
            s_got_usr1 = 0;
            process_cmd();
        }

        int status;
        pid_t dead;
        while ((dead = waitpid(-1, &status, WNOHANG)) > 0) {
            for (i = 0; i < s_nsvc; i++) {
                if (s_svcs[i].pid != dead) continue;
                s_svcs[i].pid = -1;
                if (!s_svcs[i].active || s_svcs[i].policy == POLICY_ONESHOT)
                    break;
                if (s_svcs[i].restarts >= s_svcs[i].max_restarts) {
                    vigil_log(s_svcs[i].name);
                    s_svcs[i].active = 0;
                    break;
                }
                s_svcs[i].restarts++;
                start_service(&s_svcs[i]);
                break;
            }
        }

        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }

    shutdown_all();
    sync();
    vigil_log("halted");
    return 0;
}
