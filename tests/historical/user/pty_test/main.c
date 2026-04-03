#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

int main(void)
{
    /* Step 1: Open master */
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        printf("PTY FAIL: open /dev/ptmx errno=%d\n", errno);
        return 1;
    }
    printf("pty_test: master fd=%d\n", master);

    /* Step 2: Get PTY number */
    unsigned int ptyno = 99;
    if (ioctl(master, 0x80045430, &ptyno) < 0) {
        printf("PTY FAIL: TIOCGPTN errno=%d\n", errno);
        return 1;
    }
    printf("pty_test: ptyno=%u\n", ptyno);

    /* Step 3: Unlock */
    int zero = 0;
    if (ioctl(master, 0x40045431, &zero) < 0) {
        printf("PTY FAIL: TIOCSPTLCK errno=%d\n", errno);
        return 1;
    }
    printf("pty_test: unlocked\n");

    /* Step 4: Open slave */
    char pts_path[32];
    snprintf(pts_path, sizeof(pts_path), "/dev/pts/%u", ptyno);
    printf("pty_test: opening %s\n", pts_path);

    pid_t pid = fork();
    if (pid < 0) {
        printf("PTY FAIL: fork\n");
        return 1;
    }
    if (pid == 0) {
        /* Child: open slave, write, exit */
        close(master);
        int slave = open(pts_path, O_RDWR);
        if (slave < 0) {
            /* Can't use printf here easily, just exit with error code */
            _exit(2);
        }
        int w = write(slave, "hello\n", 6);
        if (w < 0) _exit(3);
        close(slave);
        _exit(0);
    }

    /* Parent: wait for child then read */
    int status;
    waitpid(pid, &status, 0);
    int child_exit = (status >> 8) & 0xFF;
    printf("pty_test: child exited %d\n", child_exit);

    if (child_exit != 0) {
        printf("PTY FAIL: child failed (exit=%d, open or write error)\n", child_exit);
        close(master);
        return 1;
    }

    /* Read from master — data should be in output_buf */
    char buf[128];
    int n = read(master, buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
    else buf[0] = '\0';

    printf("pty_test: master read n=%d\n", n);

    if (n <= 0 || !strstr(buf, "hello")) {
        printf("PTY FAIL: master read got %d bytes: '%s'\n", n, buf);
        close(master);
        return 1;
    }

    close(master);
    printf("PTY OK\n");
    return 0;
}
