#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void *thread_fn(void *arg)
{
    int *val = (int *)arg;
    *val = 42;
    return (void *)0;
}

int main(void)
{
    /* Test 1: basic pthread create + join */
    int result = 0;
    pthread_t t;
    int err = pthread_create(&t, NULL, thread_fn, &result);
    if (err != 0) {
        printf("THREAD FAIL: create err=%d\n", err);
        return 1;
    }
    if (pthread_join(t, NULL) != 0) {
        printf("THREAD FAIL: join\n");
        return 1;
    }
    if (result == 42)
        printf("THREAD OK\n");
    else
        printf("THREAD FAIL: result=%d\n", result);

    return 0;
}
