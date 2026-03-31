/* user/capd_test/main.c — capd integration test binary.
 *
 * Binary protocol: sends uint32_t kind, reads int32_t result.
 *
 * Tests:
 * 1. Request NET_ADMIN (allowed) — verify result=0
 * 2. Query own caps — verify NET_ADMIN present
 * 3. Request invalid kind (99) — verify negative result
 * 4. Request AUTH (not in policy) — verify negative result
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>

#define SYS_CAP_QUERY 362
#define SOCK_PATH     "/run/capd.sock"
#define CAP_TABLE_SIZE 16

typedef struct { uint32_t kind; uint32_t rights; } cap_slot_t;

static int
capd_request(uint32_t kind, int32_t *result)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    for (int retry = 0; retry < 10; retry++) {
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            goto connected;
        usleep(200000);
    }
    close(sock);
    return -1;
connected:;
    write(sock, &kind, sizeof(kind));
    int32_t res = -999;
    ssize_t n = read(sock, &res, sizeof(res));
    close(sock);
    if (n != sizeof(res)) return -1;
    *result = res;
    return 0;
}

static int
has_cap(int kind)
{
    cap_slot_t slots[CAP_TABLE_SIZE];
    memset(slots, 0, sizeof(slots));
    long n = syscall(SYS_CAP_QUERY, 0L, slots, sizeof(slots));
    if (n < 0) return 0;
    for (int i = 0; i < (int)n; i++)
        if ((int)slots[i].kind == kind)
            return 1;
    return 0;
}

int
main(void)
{
    int pass = 0, fail = 0;
    int32_t res;

    /* Wait for capd to finish processing login/dhcp requests */
    usleep(500000);

    /* Test 1: Request NET_ADMIN (allowed by policy) */
    printf("test 1: request NET_ADMIN... ");
    if (capd_request(8, &res) == 0 && res >= 0) {
        printf("OK\n");
        pass++;
    } else {
        printf("FAIL (res=%d)\n", res);
        fail++;
    }

    /* Test 2: Verify NET_ADMIN was actually granted */
    printf("test 2: verify NET_ADMIN in caps... ");
    if (has_cap(8)) {
        printf("OK\n");
        pass++;
    } else {
        printf("FAIL\n");
        fail++;
    }

    /* Test 3: Request invalid kind (99) */
    printf("test 3: request kind 99... ");
    if (capd_request(99, &res) == 0 && res < 0) {
        printf("OK (res=%d)\n", res);
        pass++;
    } else {
        printf("FAIL (res=%d)\n", res);
        fail++;
    }

    /* Test 4: Request AUTH (not in capd_test.policy) */
    printf("test 4: request AUTH (disallowed)... ");
    if (capd_request(4, &res) == 0 && res < 0) {
        printf("OK (res=%d)\n", res);
        pass++;
    } else {
        printf("FAIL (res=%d)\n", res);
        fail++;
    }

    if (fail == 0)
        printf("CAPD TESTS PASSED (%d/%d)\n", pass, pass);
    else
        printf("CAPD TESTS FAILED (%d passed, %d failed)\n", pass, fail);

    return fail ? 1 : 0;
}
