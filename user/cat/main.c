#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { write(2, "cat: missing operand\n", 21); return 1; }
    int i;
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) { perror(argv[i]); return 1; }
        char buf[512];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }
    return 0;
}
