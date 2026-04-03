#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

static int test_maps(void)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/self/maps\n");
        return 1;
    }
    char buf[2048];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/self/maps empty\n");
        return 1;
    }
    buf[n] = '\0';
    if (!strstr(buf, "[stack]")) {
        printf("PROC FAIL: /proc/self/maps missing [stack]\n");
        printf("  got: %s\n", buf);
        return 1;
    }
    return 0;
}

static int test_status(void)
{
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/self/status\n");
        return 1;
    }
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/self/status empty\n");
        return 1;
    }
    buf[n] = '\0';
    pid_t pid = getpid();
    char needle[32];
    snprintf(needle, sizeof(needle), "Pid:\t%d\n", (int)pid);
    if (!strstr(buf, needle)) {
        printf("PROC FAIL: /proc/self/status missing '%s'\n", needle);
        printf("  got: %s\n", buf);
        return 1;
    }
    return 0;
}

static int test_meminfo(void)
{
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/meminfo\n");
        return 1;
    }
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/meminfo empty\n");
        return 1;
    }
    buf[n] = '\0';
    if (!strstr(buf, "MemTotal:")) {
        printf("PROC FAIL: /proc/meminfo missing MemTotal\n");
        return 1;
    }
    return 0;
}

static int test_stat(void)
{
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) {
        printf("PROC FAIL: open /proc/self/stat\n");
        return 1;
    }
    char buf[512];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("PROC FAIL: read /proc/self/stat empty\n");
        return 1;
    }
    buf[n] = '\0';
    pid_t pid = getpid();
    int got_pid = 0;
    sscanf(buf, "%d", &got_pid);
    if (got_pid != (int)pid) {
        printf("PROC FAIL: /proc/self/stat pid=%d expected=%d\n",
               got_pid, (int)pid);
        return 1;
    }
    return 0;
}

static int test_fd_dir(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) {
        printf("PROC FAIL: opendir /proc/self/fd\n");
        return 1;
    }
    int found_0 = 0, found_1 = 0, found_2 = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "0") == 0) found_0 = 1;
        if (strcmp(de->d_name, "1") == 0) found_1 = 1;
        if (strcmp(de->d_name, "2") == 0) found_2 = 1;
    }
    closedir(d);
    if (!found_0 || !found_1 || !found_2) {
        printf("PROC FAIL: /proc/self/fd missing stdin/stdout/stderr (%d%d%d)\n",
               found_0, found_1, found_2);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_maps()) return 1;
    if (test_status()) return 1;
    if (test_meminfo()) return 1;
    if (test_stat()) return 1;
    if (test_fd_dir()) return 1;
    printf("PROC OK\n");
    return 0;
}
