/* Minimal ARM64 init for Aegis — uses musl libc.
 * Compiled with: gcc -static -O2 -o init init.c */
#include <unistd.h>
#include <string.h>

int main(void) {
    const char msg[] = "[INIT] Hello from ARM64 musl libc!\n";
    write(1, msg, sizeof(msg) - 1);
    _exit(0);
    return 0;
}
