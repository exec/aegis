#!/bin/bash
# Build ALL ARM64 musl-static user binaries inside Docker.
# Usage: bash tools/build-arm64-userland.sh
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LINK_BASE=0x41000000

echo "Building ARM64 user binaries via Docker (alpine:latest)..."

docker run --rm --platform linux/arm64 \
    -v "$REPO:/aegis" \
    alpine:latest \
    sh -c "
        set -e
        apk add --no-cache gcc musl-dev make xxd >/dev/null 2>&1

        # Common CFLAGS for all user binaries
        CFLAGS='-static -O2 -no-pie -fno-pie -Wall -Wextra'
        LDFLAGS=\"-Wl,-Ttext-segment=$LINK_BASE\"

        build_simple() {
            local name=\$1
            local src=\$2
            echo \"  Building \$name...\"
            gcc \$CFLAGS \$LDFLAGS -o /aegis/user/\$name/\$name.elf \$src 2>&1 || echo \"FAIL: \$name\"
        }

        # Simple single-file programs
        for prog in cat echo pwd uname clear mkdir touch rm cp mv whoami; do
            if [ -f /aegis/user/\$prog/main.c ]; then
                build_simple \$prog /aegis/user/\$prog/main.c
            elif [ -f /aegis/user/\$prog/\${prog}.c ]; then
                build_simple \$prog /aegis/user/\$prog/\${prog}.c
            fi
        done

        # Programs with specific source files
        [ -f /aegis/user/ls/main.c ] && build_simple ls /aegis/user/ls/main.c
        [ -f /aegis/user/wc/main.c ] && build_simple wc /aegis/user/wc/main.c
        [ -f /aegis/user/grep/main.c ] && build_simple grep /aegis/user/grep/main.c
        [ -f /aegis/user/sort/main.c ] && build_simple sort /aegis/user/sort/main.c
        [ -f /aegis/user/login/main.c ] && build_simple login /aegis/user/login/main.c
        [ -f /aegis/user/shell/main.c ] && build_simple shell /aegis/user/shell/main.c

        # true/false
        [ -f /aegis/user/true/main.c ] && build_simple true /aegis/user/true/main.c
        [ -f /aegis/user/false/main.c ] && build_simple false /aegis/user/false/main.c

        # init
        [ -f /aegis/user/init-arm64/init_musl.c ] && \
            gcc \$CFLAGS \$LDFLAGS -o /aegis/user/init-arm64/init-musl.elf /aegis/user/init-arm64/init_musl.c

        echo '=== Generating xxd blobs ==='
        cd /aegis

        # Generate init blob
        xxd -i user/init-arm64/init-musl.elf | \
            sed 's/user_init_arm64_init_musl_elf/init_elf/g; s/unsigned char/const unsigned char/; s/unsigned int/const unsigned int/' \
            > kernel/arch/arm64/init_arm64_bin.c

        # Generate shell blob for ARM64
        if [ -f user/shell/shell.elf ]; then
            xxd -i user/shell/shell.elf | \
                sed 's/user_shell_shell_elf/shell_elf/g; s/unsigned char/const unsigned char/; s/unsigned int/const unsigned int/' \
                > kernel/arch/arm64/shell_arm64_bin.c
            echo 'Generated shell_arm64_bin.c'
        fi

        echo 'Done!'
    "

echo "ARM64 userland build complete."
