/* ipc_test — Phase 44 IPC test binary */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>

/* memfd_create syscall — not in musl headers */
static int memfd_create(const char *name, unsigned int flags)
{
    return (int)syscall(319, name, flags);
}

#define SOCK_PATH "/tmp/ipc_test.sock"

static int test_passed = 0;
static int test_total  = 0;

static void pass(const char *name)
{
    test_passed++;
    test_total++;
    printf("  PASS  %s\n", name);
}

static void fail(const char *name, const char *reason)
{
    test_total++;
    printf("  FAIL  %s: %s\n", name, reason);
}

/* Test 1: Unix socket echo — server bind/listen/accept, client connect, round-trip */
static void test_unix_echo(void)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { fail("unix_echo", "socket()"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCK_PATH);  /* may fail, that's fine */
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fail("unix_echo", "bind()");
        close(srv);
        return;
    }
    if (listen(srv, 5) < 0) {
        fail("unix_echo", "listen()");
        close(srv);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: client */
        close(srv);
        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cli < 0) _exit(1);

        struct sockaddr_un caddr;
        memset(&caddr, 0, sizeof(caddr));
        caddr.sun_family = AF_UNIX;
        strncpy(caddr.sun_path, SOCK_PATH, sizeof(caddr.sun_path) - 1);

        if (connect(cli, (struct sockaddr *)&caddr, sizeof(caddr)) < 0) _exit(2);

        const char *msg = "hello IPC";
        write(cli, msg, strlen(msg));

        char buf[64];
        int n = read(cli, buf, sizeof(buf));
        if (n > 0) buf[n] = '\0';
        close(cli);
        _exit(n > 0 && strcmp(buf, "hello IPC") == 0 ? 0 : 3);
    }

    /* Parent: server */
    int conn = accept(srv, NULL, NULL);
    if (conn < 0) {
        fail("unix_echo", "accept()");
        close(srv);
        waitpid(pid, NULL, 0);
        return;
    }

    char buf[64];
    int n = read(conn, buf, sizeof(buf));
    if (n > 0) {
        write(conn, buf, n);  /* echo back */
    }
    close(conn);
    close(srv);

    int status;
    waitpid(pid, &status, 0);
    if (status == 0)
        pass("unix_echo");
    else
        fail("unix_echo", "child failed");
}

/* Test 2: SO_PEERCRED */
static void test_peercred(void)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { fail("peercred", "socket()"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/ipc_cred.sock", sizeof(addr.sun_path) - 1);

    unlink("/tmp/ipc_cred.sock");
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fail("peercred", "bind()");
        close(srv);
        return;
    }
    listen(srv, 5);

    pid_t pid = fork();
    if (pid == 0) {
        close(srv);
        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un caddr;
        memset(&caddr, 0, sizeof(caddr));
        caddr.sun_family = AF_UNIX;
        strncpy(caddr.sun_path, "/tmp/ipc_cred.sock", sizeof(caddr.sun_path) - 1);
        connect(cli, (struct sockaddr *)&caddr, sizeof(caddr));
        /* Keep connection alive briefly */
        char c;
        read(cli, &c, 1);
        close(cli);
        _exit(0);
    }

    int conn = accept(srv, NULL, NULL);
    if (conn < 0) {
        fail("peercred", "accept()");
        close(srv);
        waitpid(pid, NULL, 0);
        return;
    }

    struct ucred {
        int pid;
        int uid;
        int gid;
    } cred;
    socklen_t len = sizeof(cred);
    int rc = getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len);

    /* Signal child to exit */
    write(conn, "x", 1);
    close(conn);
    close(srv);
    waitpid(pid, NULL, 0);

    if (rc == 0 && cred.pid > 0)
        pass("peercred");
    else
        fail("peercred", "getsockopt failed or pid=0");
}

/* Test 3: fd passing via sendmsg/recvmsg SCM_RIGHTS */
static void test_fd_passing(void)
{
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { fail("fd_passing", "socket()"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/ipc_fdpass.sock", sizeof(addr.sun_path) - 1);

    unlink("/tmp/ipc_fdpass.sock");
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 5);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: sender */
        close(srv);
        int cli = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un caddr;
        memset(&caddr, 0, sizeof(caddr));
        caddr.sun_family = AF_UNIX;
        strncpy(caddr.sun_path, "/tmp/ipc_fdpass.sock", sizeof(caddr.sun_path) - 1);
        connect(cli, (struct sockaddr *)&caddr, sizeof(caddr));

        /* Create a memfd, write magic to it */
        int mfd = memfd_create("test", 0);
        if (mfd < 0) _exit(1);
        ftruncate(mfd, 4096);

        /* Write magic via mmap */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mfd, 0);
        if (p == MAP_FAILED) _exit(2);
        *(volatile unsigned int *)p = 0xDEADBEEF;
        munmap(p, 4096);

        /* Send the memfd via SCM_RIGHTS */
        struct msghdr msg;
        struct iovec iov;
        char data = 'F';
        char cmsgbuf[CMSG_SPACE(sizeof(int))];

        iov.iov_base = &data;
        iov.iov_len = 1;

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &mfd, sizeof(int));

        sendmsg(cli, &msg, 0);
        close(mfd);
        close(cli);
        _exit(0);
    }

    /* Parent: receiver */
    int conn = accept(srv, NULL, NULL);
    if (conn < 0) {
        fail("fd_passing", "accept()");
        close(srv);
        waitpid(pid, NULL, 0);
        return;
    }

    struct msghdr msg;
    struct iovec iov;
    char data;
    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    iov.iov_base = &data;
    iov.iov_len = 1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    int n = recvmsg(conn, &msg, 0);
    close(conn);
    close(srv);
    waitpid(pid, NULL, 0);

    if (n <= 0) { fail("fd_passing", "recvmsg failed"); return; }

    /* Extract the passed fd */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        fail("fd_passing", "no SCM_RIGHTS in cmsg");
        return;
    }

    int received_fd;
    memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));

    /* mmap the received memfd and check the magic */
    void *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, received_fd, 0);
    if (p == MAP_FAILED) {
        fail("fd_passing", "mmap of received fd failed");
        close(received_fd);
        return;
    }

    unsigned int magic = *(volatile unsigned int *)p;
    munmap(p, 4096);
    close(received_fd);

    if (magic == 0xDEADBEEF)
        pass("fd_passing");
    else
        fail("fd_passing", "magic mismatch");
}

/* Test 4: MAP_SHARED cross-process */
static void test_map_shared(void)
{
    int mfd = memfd_create("shared", 0);
    if (mfd < 0) { fail("map_shared", "memfd_create"); return; }
    if (ftruncate(mfd, 4096) < 0) { fail("map_shared", "ftruncate"); close(mfd); return; }

    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (p == MAP_FAILED) { fail("map_shared", "mmap"); close(mfd); return; }

    *(volatile unsigned int *)p = 0;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: map the same memfd (inherited fd) and read */
        void *cp = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0);
        if (cp == MAP_FAILED) _exit(1);

        /* Wait for parent to write (simple spin — no signals needed) */
        volatile unsigned int *vp = (volatile unsigned int *)cp;
        int tries = 0;
        while (*vp == 0 && tries < 1000000) tries++;

        unsigned int val = *vp;
        munmap(cp, 4096);
        close(mfd);
        _exit(val == 0x12345678 ? 0 : 2);
    }

    /* Parent: write to shared mapping */
    /* Small delay to let child mmap first */
    for (volatile int i = 0; i < 100000; i++) {}
    *(volatile unsigned int *)p = 0x12345678;

    int status;
    waitpid(pid, &status, 0);
    munmap(p, 4096);
    close(mfd);

    if (status == 0)
        pass("map_shared");
    else
        fail("map_shared", "child saw wrong value");
}

/* Test 5: Error cases */
static void test_errors(void)
{
    /* Connect to nonexistent path should return ECONNREFUSED */
    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli < 0) { fail("errors", "socket()"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/nonexistent.sock", sizeof(addr.sun_path) - 1);

    int rc = connect(cli, (struct sockaddr *)&addr, sizeof(addr));
    close(cli);

    if (rc < 0)
        pass("errors");
    else
        fail("errors", "connect to nonexistent should fail");
}

int main(void)
{
    printf("IPC test starting...\n");

    test_unix_echo();
    test_peercred();
    test_fd_passing();
    test_map_shared();
    test_errors();

    if (test_passed == test_total)
        printf("ALL %d IPC TESTS PASSED\n", test_total);
    else
        printf("%d/%d IPC TESTS PASSED\n", test_passed, test_total);

    return test_passed == test_total ? 0 : 1;
}
