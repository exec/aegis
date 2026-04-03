#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cp <src> <dst>\n");
        return 1;
    }
    int src = open(argv[1], O_RDONLY);
    if (src < 0) { perror("cp: open src"); return 1; }
    int dst = open(argv[2], O_WRONLY | O_CREAT, 0644);
    if (dst < 0) { perror("cp: open dst"); close(src); return 1; }
    char buf[4096];
    int n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        int w = write(dst, buf, n);
        if (w != n) { perror("cp: write"); break; }
    }
    close(src);
    close(dst);
    return 0;
}
