#include <sys/mman.h>
#include <stdio.h>

int main(void)
{
    /* Test 1: VA reuse — munmap then mmap should recycle the address. */
    void *a1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a1 == MAP_FAILED) {
        printf("MMAP FAIL: mmap1\n");
        return 1;
    }
    *(volatile int *)a1 = 99;
    munmap(a1, 4096);

    void *a2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (a2 == MAP_FAILED) {
        printf("MMAP FAIL: mmap2\n");
        return 1;
    }
    if (a2 != a1) {
        printf("MMAP FAIL: VA not reused a1=%p a2=%p\n", a1, a2);
        return 1;
    }

    /* Test 2: mprotect changes permissions — write to read-only, then
     * restore writable. No SIGSEGV test (kernel doesn't deliver hardware
     * exceptions as signals yet). Instead verify mprotect returns 0 and
     * that a read-then-write cycle works after restoring permissions. */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        printf("MMAP FAIL: mmap3\n");
        return 1;
    }
    *(volatile int *)p = 42;

    /* Make read-only */
    if (mprotect(p, 4096, PROT_READ) != 0) {
        printf("MMAP FAIL: mprotect PROT_READ\n");
        return 1;
    }

    /* Read should succeed */
    volatile int val = *(volatile int *)p;
    if (val != 42) {
        printf("MMAP FAIL: read after PROT_READ got %d\n", val);
        return 1;
    }

    /* Restore writable and write again */
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("MMAP FAIL: mprotect PROT_RW\n");
        return 1;
    }
    *(volatile int *)p = 99;
    if (*(volatile int *)p != 99) {
        printf("MMAP FAIL: write after PROT_RW\n");
        return 1;
    }

    /* Test 3: multi-page munmap + mmap VA coalescing.
     * Allocate two adjacent pages, munmap both, then mmap a 2-page
     * region. The freelist should coalesce the two entries and return
     * the original base address. */
    void *m1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    void *m2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (m1 == MAP_FAILED || m2 == MAP_FAILED) {
        printf("MMAP FAIL: mmap coalesce alloc\n");
        return 1;
    }
    /* These should be adjacent (bump allocator) */
    if ((char *)m2 != (char *)m1 + 4096) {
        printf("MMAP FAIL: not adjacent\n");
        return 1;
    }
    munmap(m1, 4096);
    munmap(m2, 4096);
    /* Now request 2 pages — should get m1 back (coalesced region) */
    void *m3 = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (m3 == MAP_FAILED) {
        printf("MMAP FAIL: mmap coalesce realloc\n");
        return 1;
    }
    if (m3 != m1) {
        printf("MMAP FAIL: coalesce reuse m1=%p m3=%p\n", m1, m3);
        return 1;
    }

    printf("MMAP OK\n");
    return 0;
}
