#include "tty.h"
#include "printk.h"
#include "uaccess.h"
#include "signal.h"
#include "proc.h"
#include "sched.h"
#include "syscall_util.h"
#include <stdint.h>

/* ── Console tty singleton (set by console driver) ───────────────── */

static tty_t *s_console_tty;

tty_t *tty_console(void)
{
    return s_console_tty;
}

/* Called by the console driver to register the console tty. */
void tty_set_console(tty_t *tty)
{
    s_console_tty = tty;
}

tty_t *tty_find_controlling(uint32_t session_id)
{
    /* Phase 32 stub: only the console tty exists. */
    if (s_console_tty && s_console_tty->session_id == session_id)
        return s_console_tty;
    return (tty_t *)0;
}

/* ── Defaults ────────────────────────────────────────────────────── */

void tty_init_defaults(tty_t *tty)
{
    __builtin_memset(tty, 0, sizeof(*tty));
    tty->termios.c_iflag = K_ICRNL;
    tty->termios.c_oflag = K_OPOST | K_ONLCR;
    tty->termios.c_lflag = K_ICANON | K_ECHO | K_ISIG;
    tty->termios.c_cc[K_VINTR]  = 0x03; /* Ctrl-C */
    tty->termios.c_cc[K_VQUIT]  = 0x1C; /* Ctrl-\ */
    tty->termios.c_cc[K_VERASE] = 0x7F; /* DEL */
    tty->termios.c_cc[K_VEOF]   = 0x04; /* Ctrl-D */
    tty->termios.c_cc[K_VSUSP]  = 0x1A; /* Ctrl-Z */
    tty->termios.c_cc[K_VMIN]   = 1;
    tty->termios.c_cc[K_VTIME]  = 0;
    tty->rows = 25;
    tty->cols = 80;
}

/* ── Foreground check ────────────────────────────────────────────── */

int tty_is_fg(tty_t *tty)
{
    aegis_task_t *t;
    aegis_process_t *proc;

    if (tty->fg_pgrp == 0)
        return 1;

    t = sched_current();
    if (!t || !t->is_user)
        return 1;

    proc = (aegis_process_t *)t;
    return proc->pgid == tty->fg_pgrp;
}

/* ── Echo helper ─────────────────────────────────────────────────── */

static void tty_echo(tty_t *tty, const char *s, uint32_t n)
{
    if (tty->termios.c_lflag & K_ECHO)
        tty->write_out(tty, s, n);
}

/* ── Read ────────────────────────────────────────────────────────── */

int tty_read(tty_t *tty, char *buf, uint32_t len)
{
    uint32_t copied = 0;
    char ch;
    int interrupted;

    if (len == 0)
        return 0;

    /* SIGTTIN: background process trying to read */
    if (!tty_is_fg(tty)) {
        aegis_task_t *t = sched_current();
        if (t && t->is_user) {
            aegis_process_t *proc = (aegis_process_t *)t;
            signal_send_pgrp(proc->pgid, SIGTTIN);
        }
        return -4; /* EINTR */
    }

    /* RAW mode: return one byte at a time */
    if (!(tty->termios.c_lflag & K_ICANON)) {
        interrupted = 0;
        int rc = tty->read_raw(tty, &ch, &interrupted);
        if (rc <= 0)
            return interrupted ? -4 : rc;
        /* CR→NL if ICRNL */
        if ((tty->termios.c_iflag & K_ICRNL) && ch == '\r')
            ch = '\n';
        buf[0] = ch;
        return 1;
    }

    /* COOKED mode: return from line buffer if data available */
    if (tty->line_pos < tty->line_len) {
        while (copied < len && tty->line_pos < tty->line_len) {
            buf[copied++] = tty->linebuf[tty->line_pos++];
        }
        /* Reset buffer when fully consumed */
        if (tty->line_pos >= tty->line_len) {
            tty->line_len = 0;
            tty->line_pos = 0;
            tty->line_ready = 0;
        }
        return (int)copied;
    }

    /* Fill line buffer by reading raw characters */
    tty->line_len = 0;
    tty->line_pos = 0;
    tty->line_ready = 0;

    while (!tty->line_ready) {
        interrupted = 0;
        int rc = tty->read_raw(tty, &ch, &interrupted);
        if (rc <= 0) {
            if (interrupted)
                return -4; /* EINTR */
            return rc;
        }

        /* CR→NL translation */
        if ((tty->termios.c_iflag & K_ICRNL) && ch == '\r')
            ch = '\n';

        /* Signal generation (ISIG) */
        if (tty->termios.c_lflag & K_ISIG) {
            if (ch == (char)tty->termios.c_cc[K_VINTR]) {
                tty_echo(tty, "^C", 2);
                tty->write_out(tty, "\n", 1);
                tty->line_len = 0;
                if (tty->fg_pgrp)
                    signal_send_pgrp(tty->fg_pgrp, SIGINT);
                return -4;
            }
            if (ch == (char)tty->termios.c_cc[K_VSUSP]) {
                tty_echo(tty, "^Z", 2);
                tty->write_out(tty, "\n", 1);
                tty->line_len = 0;
                if (tty->fg_pgrp)
                    signal_send_pgrp(tty->fg_pgrp, SIGTSTP);
                return -4;
            }
            if (ch == (char)tty->termios.c_cc[K_VQUIT]) {
                tty_echo(tty, "^\\", 2);
                tty->write_out(tty, "\n", 1);
                tty->line_len = 0;
                if (tty->fg_pgrp)
                    signal_send_pgrp(tty->fg_pgrp, SIGQUIT);
                return -4;
            }
        }

        /* Newline → flush line */
        if (ch == '\n') {
            if (tty->line_len < sizeof(tty->linebuf))
                tty->linebuf[tty->line_len++] = '\n';
            tty_echo(tty, "\n", 1);
            tty->line_ready = 1;
            break;
        }

        /* EOF (Ctrl-D) */
        if (ch == (char)tty->termios.c_cc[K_VEOF]) {
            if (tty->line_len == 0)
                return 0; /* EOF: no data */
            /* Flush what we have without adding EOF char */
            tty->line_ready = 1;
            break;
        }

        /* Erase (backspace / DEL) */
        if (ch == (char)tty->termios.c_cc[K_VERASE] || ch == '\b') {
            if (tty->line_len > 0) {
                tty->line_len--;
                tty_echo(tty, "\b \b", 3);
            }
            continue;
        }

        /* Printable characters */
        if ((uint8_t)ch >= 0x20) {
            if (tty->line_len < sizeof(tty->linebuf)) {
                tty->linebuf[tty->line_len++] = ch;
                tty_echo(tty, &ch, 1);
            }
        }
    }

    /* Copy from line buffer to user */
    tty->line_pos = 0;
    while (copied < len && tty->line_pos < tty->line_len) {
        buf[copied++] = tty->linebuf[tty->line_pos++];
    }
    if (tty->line_pos >= tty->line_len) {
        tty->line_len = 0;
        tty->line_pos = 0;
        tty->line_ready = 0;
    }
    return (int)copied;
}

/* ── Write ───────────────────────────────────────────────────────── */

int tty_write(tty_t *tty, const char *buf, uint32_t len)
{
    uint32_t i;

    /* SIGTTOU: background process writing with TOSTOP set */
    if ((tty->termios.c_lflag & K_TOSTOP) && !tty_is_fg(tty)) {
        aegis_task_t *t = sched_current();
        if (t && t->is_user) {
            aegis_process_t *proc = (aegis_process_t *)t;
            signal_send_pgrp(proc->pgid, SIGTTOU);
        }
        return -4; /* EINTR */
    }

    /* Output processing: OPOST + ONLCR maps \n → \r\n */
    if ((tty->termios.c_oflag & K_OPOST) &&
        (tty->termios.c_oflag & K_ONLCR)) {
        uint32_t start = 0;
        for (i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                if (i > start)
                    tty->write_out(tty, buf + start, i - start);
                tty->write_out(tty, "\r\n", 2);
                start = i + 1;
            }
        }
        if (start < len)
            tty->write_out(tty, buf + start, len - start);
        return (int)len;
    }

    return tty->write_out(tty, buf, len);
}

/* ── Ioctl ───────────────────────────────────────────────────────── */

int tty_ioctl(tty_t *tty, uint32_t cmd, uint64_t arg)
{
    switch (cmd) {
    case TCGETS:
        if (!user_ptr_valid(arg, sizeof(k_termios_t)))
            return -14; /* EFAULT */
        copy_to_user((void *)arg, &tty->termios, sizeof(k_termios_t));
        return 0;

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!user_ptr_valid(arg, sizeof(k_termios_t)))
            return -14; /* EFAULT */
        copy_from_user(&tty->termios, (const void *)arg, sizeof(k_termios_t));
        return 0;

    case TIOCGPGRP: {
        uint32_t val = tty->fg_pgrp;
        if (!user_ptr_valid(arg, sizeof(uint32_t)))
            return -14;
        copy_to_user((void *)arg, &val, sizeof(uint32_t));
        return 0;
    }

    case TIOCSPGRP: {
        uint32_t val;
        if (!user_ptr_valid(arg, sizeof(uint32_t)))
            return -14;
        copy_from_user(&val, (const void *)arg, sizeof(uint32_t));
        tty->fg_pgrp = val;
        return 0;
    }

    case TIOCGWINSZ: {
        uint16_t ws[4];
        if (!user_ptr_valid(arg, sizeof(ws)))
            return -14;
        ws[0] = tty->rows;
        ws[1] = tty->cols;
        ws[2] = 0; /* xpixel */
        ws[3] = 0; /* ypixel */
        copy_to_user((void *)arg, ws, sizeof(ws));
        return 0;
    }

    case TIOCSWINSZ: {
        uint16_t ws[4];
        if (!user_ptr_valid(arg, sizeof(ws)))
            return -14;
        copy_from_user(ws, (const void *)arg, sizeof(ws));
        tty->rows = ws[0];
        tty->cols = ws[1];
        if (tty->fg_pgrp)
            signal_send_pgrp(tty->fg_pgrp, SIGWINCH);
        return 0;
    }

    default:
        return -25; /* ENOTTY */
    }
}
