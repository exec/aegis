/* user/mouse_test/main.c — USB mouse event reader
 *
 * Opens /dev/mouse and reads mouse_event_t structs.
 * Prints each event as: btn=XX dx=NN dy=NN
 * Exits after 10 events or on read error.
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    int16_t  dx;
    int16_t  dy;
    int16_t  scroll;
} mouse_event_t;

int main(void)
{
    int fd = open("/dev/mouse", O_RDONLY);
    if (fd < 0) {
        printf("mouse_test: cannot open /dev/mouse\n");
        return 1;
    }

    printf("mouse_test: listening for events\n");

    int count = 0;
    while (count < 10) {
        mouse_event_t evt;
        int n = (int)read(fd, &evt, sizeof(evt));
        if (n < (int)sizeof(evt)) break;

        printf("btn=%02x dx=%d dy=%d\n",
               (unsigned)evt.buttons, (int)evt.dx, (int)evt.dy);
        count++;
    }

    close(fd);
    printf("mouse_test: done (%d events)\n", count);
    return 0;
}
