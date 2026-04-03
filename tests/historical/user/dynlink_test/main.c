#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    printf("DYNLINK: printf works\n");

    char *p = malloc(1024);
    if (!p) {
        printf("DYNLINK FAIL: malloc returned NULL\n");
        return 1;
    }
    strcpy(p, "heap works");
    printf("DYNLINK: %s\n", p);
    free(p);

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        printf("DYNLINK FAIL: cannot open /proc/self/maps\n");
        return 1;
    }
    char line[256];
    int found_interp = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "40000000"))
            found_interp = 1;
    }
    fclose(f);

    if (found_interp)
        printf("DYNLINK: interpreter mapping found at 0x40000000\n");
    else {
        printf("DYNLINK FAIL: no mapping at 0x40000000\n");
        return 1;
    }

    printf("DYNLINK OK\n");
    return 0;
}
