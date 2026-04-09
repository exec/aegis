/* config.c — grub.cfg writer + test binary strip (libinstall) */
#include "libinstall.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#ifndef O_TRUNC
#define O_TRUNC 0x200
#endif

int install_write_grub_cfg(void)
{
    int fd = open("/boot/grub/grub.cfg", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;

    /* Installed-system grub.cfg — loaded by GRUB from the ext2 root
     * partition on the installed disk. Paths are relative to ext2
     * root, not the ESP prefix. Default is graphical mode. */
    const char *cfg =
        "set timeout=3\n"
        "set default=0\n"
        "\n"
        "insmod all_video\n"
        "insmod gfxterm\n"
        "insmod png\n"
        "insmod font\n"
        "\n"
        "set gfxmode=1024x768x32,800x600x32,auto\n"
        "if loadfont /boot/grub/font.pf2; then\n"
        "    true\n"
        "fi\n"
        "terminal_input console\n"
        "terminal_output gfxterm\n"
        "\n"
        "if background_image /boot/grub/wallpaper.png; then\n"
        "    true\n"
        "fi\n"
        "\n"
        "menuentry \"Aegis (graphical)\" {\n"
        "    set gfxpayload=keep\n"
        "    multiboot2 /boot/aegis.elf boot=graphical quiet\n"
        "    boot\n"
        "}\n"
        "\n"
        "menuentry \"Aegis (text)\" {\n"
        "    set gfxpayload=keep\n"
        "    multiboot2 /boot/aegis.elf boot=text quiet\n"
        "    boot\n"
        "}\n"
        "\n"
        "menuentry \"Aegis (debug)\" {\n"
        "    set gfxpayload=keep\n"
        "    multiboot2 /boot/aegis.elf boot=text\n"
        "    boot\n"
        "}\n";
    ssize_t w = write(fd, cfg, strlen(cfg));
    close(fd);
    return (w > 0) ? 0 : -1;
}

void install_strip_test_binaries(void)
{
    unlink("/bin/thread_test");
    unlink("/bin/mmap_test");
    unlink("/bin/proc_test");
    unlink("/bin/pty_test");
    unlink("/bin/dynlink_test");
}
