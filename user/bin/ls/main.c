#include <stdio.h>
#include <dirent.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { perror(path); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        puts(e->d_name);
    closedir(d);
    return 0;
}
