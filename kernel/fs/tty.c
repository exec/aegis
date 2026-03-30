#include "tty.h"
#include "pty.h"
#include "printk.h"
#include "uaccess.h"
#include "signal.h"
#include "proc.h"
#include "sched.h"
#include "syscall_util.h"
#include "../core/spinlock.h"
#include <stdint.h>

static spinlock_t tty_global_lock = SPINLOCK_INIT;

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
    tty_t *t;

    /* Check the console tty first. */
    if (s_console_tty && s_console_tty->session_id == session_id)
        return s_console_tty;
    /* Search the PTY pool. */
    t = pty_find_by_session(session_id);
    if (t)
        return t;
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
    irqflags_t fl;

    if (len == 0)
        return 0;

    /* SIGTTIN: background process trying to read.
     * If the process ignores SIGTTIN (e.g. a compositor that always
     * needs keyboard input), allow the read to proceed. */
    if (!tty_is_fg(tty)) {
        aegis_task_t *t = sched_current();
        if (t && t->is_user) {
            aegis_process_t *proc = (aegis_process_t *)t;
            if (proc->sigactions[SIGTTIN].sa_handler != SIG_IGN) {
                signal_send_pgrp(proc->pgid, SIGTTIN);
                return -4; /* EINTR */
            }
            /* SIGTTIN ignored — allow background read */
        }
    }

    /* Snapshot termios flags under lock */
    fl = spin_lock_irqsave(&tty_global_lock);
    uint32_t lflag = tty->termios.c_lflag;
    uint32_t iflag = tty->termios.c_iflag;
    char cc_vintr  = (char)tty->termios.c_cc[K_VINTR];
    char cc_vsusp  = (char)tty->termios.c_cc[K_VSUSP];
    char cc_vquit  = (char)tty->termios.c_cc[K_VQUIT];
    char cc_veof   = (char)tty->termios.c_cc[K_VEOF];
    char cc_verase = (char)tty->termios.c_cc[K_VERASE];
    spin_unlock_irqrestore(&tty_global_lock, fl);

    /* RAW mode: return one byte at a time */
    if (!(lflag & K_ICANON)) {
        uint8_t vmin = tty->termios.c_cc[K_VMIN];

        /* VMIN=0: non-blocking — poll for available data, return 0 if none */
        if (vmin == 0 && tty->poll_raw) {
            if (!tty->poll_raw(tty, &ch))
                return 0;   /* no data — POSIX: return 0 for VMIN=0 */
        } else {
            interrupted = 0;
            int rc = tty->read_raw(tty, &ch, &interrupted);
            if (rc <= 0)
                return interrupted ? -4 : rc;
        }
        /* CR→NL if ICRNL */
        if ((iflag & K_ICRNL) && ch == '\r')
            ch = '\n';
        buf[0] = ch;
        return 1;
    }

    /* COOKED mode: return from line buffer if data available */
    fl = spin_lock_irqsave(&tty_global_lock);
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
        spin_unlock_irqrestore(&tty_global_lock, fl);
        return (int)copied;
    }

    /* Fill line buffer by reading raw characters */
    tty->line_len = 0;
    tty->line_pos = 0;
    tty->line_ready = 0;
    spin_unlock_irqrestore(&tty_global_lock, fl);

    while (1) {
        fl = spin_lock_irqsave(&tty_global_lock);
        int ready = tty->line_ready;
        spin_unlock_irqrestore(&tty_global_lock, fl);
        if (ready) break;

        interrupted = 0;
        /* read_raw may block (sched_block) — lock is NOT held */
        int rc = tty->read_raw(tty, &ch, &interrupted);
        if (rc <= 0) {
            if (interrupted)
                return -4; /* EINTR */
            return rc;
        }

        /* CR→NL translation */
        if ((iflag & K_ICRNL) && ch == '\r')
            ch = '\n';

        fl = spin_lock_irqsave(&tty_global_lock);

        /* Signal generation (ISIG) */
        if (lflag & K_ISIG) {
            if (ch == cc_vintr) {
                tty_echo(tty, "^C", 2);
                tty->write_out(tty, "\n", 1);
                tty->line_len = 0;
                uint32_t fg = tty->fg_pgrp;
                spin_unlock_irqrestore(&tty_global_lock, fl);
                if (fg)
                    signal_send_pgrp(fg, SIGINT);
                return -4;
            }
            if (ch == cc_vsusp) {
                tty_echo(tty, "^Z", 2);
                tty->write_out(tty, "\n", 1);
                tty->line_len = 0;
                uint32_t fg = tty->fg_pgrp;
                spin_unlock_irqrestore(&tty_global_lock, fl);
                if (fg)
                    signal_send_pgrp(fg, SIGTSTP);
                return -4;
            }
            if (ch == cc_vquit) {
                tty_echo(tty, "^\\", 2);
                tty->write_out(tty, "\n", 1);
                tty->line_len = 0;
                uint32_t fg = tty->fg_pgrp;
                spin_unlock_irqrestore(&tty_global_lock, fl);
                if (fg)
                    signal_send_pgrp(fg, SIGQUIT);
                return -4;
            }
        }

        /* Newline → flush line */
        if (ch == '\n') {
            if (tty->line_len < sizeof(tty->linebuf))
                tty->linebuf[tty->line_len++] = '\n';
            tty_echo(tty, "\n", 1);
            tty->line_ready = 1;
            spin_unlock_irqrestore(&tty_global_lock, fl);
            break;
        }

        /* EOF (Ctrl-D) */
        if (ch == cc_veof) {
            if (tty->line_len == 0) {
                spin_unlock_irqrestore(&tty_global_lock, fl);
                return 0; /* EOF: no data */
            }
            /* Flush what we have without adding EOF char */
            tty->line_ready = 1;
            spin_unlock_irqrestore(&tty_global_lock, fl);
            break;
        }

        /* Erase (backspace / DEL) */
        if (ch == cc_verase || ch == '\b') {
            if (tty->line_len > 0) {
                tty->line_len--;
                tty_echo(tty, "\b \b", 3);
            }
            spin_unlock_irqrestore(&tty_global_lock, fl);
            continue;
        }

        /* Printable characters */
        if ((uint8_t)ch >= 0x20) {
            if (tty->line_len < sizeof(tty->linebuf)) {
                tty->linebuf[tty->line_len++] = ch;
                tty_echo(tty, &ch, 1);
            }
        }
        spin_unlock_irqrestore(&tty_global_lock, fl);
    }

    /* Copy from line buffer to user */
    fl = spin_lock_irqsave(&tty_global_lock);
    tty->line_pos = 0;
    while (copied < len && tty->line_pos < tty->line_len) {
        buf[copied++] = tty->linebuf[tty->line_pos++];
    }
    if (tty->line_pos >= tty->line_len) {
        tty->line_len = 0;
        tty->line_pos = 0;
        tty->line_ready = 0;
    }
    spin_unlock_irqrestore(&tty_global_lock, fl);
    return (int)copied;
}

/* ── Write ───────────────────────────────────────────────────────── */

int tty_write(tty_t *tty, const char *buf, uint32_t len)
{
    uint32_t i;

    /* SIGTTOU: background process writing with TOSTOP set */
    irqflags_t fl = spin_lock_irqsave(&tty_global_lock);
    int tostop = (tty->termios.c_lflag & K_TOSTOP) && !tty_is_fg(tty);
    uint32_t oflag = tty->termios.c_oflag;
    spin_unlock_irqrestore(&tty_global_lock, fl);

    if (tostop) {
        aegis_task_t *t = sched_current();
        if (t && t->is_user) {
            aegis_process_t *proc = (aegis_process_t *)t;
            signal_send_pgrp(proc->pgid, SIGTTOU);
        }
        return -4; /* EINTR */
    }

    /* Output processing: OPOST + ONLCR maps \n → \r\n */
    if ((oflag & K_OPOST) && (oflag & K_ONLCR)) {
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
    irqflags_t fl = spin_lock_irqsave(&tty_global_lock);
    int rc;
    switch (cmd) {
    case TCGETS:
        if (!user_ptr_valid(arg, sizeof(k_termios_t)))
            { rc = -14; break; }
        copy_to_user((void *)arg, &tty->termios, sizeof(k_termios_t));
        rc = 0; break;

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!user_ptr_valid(arg, sizeof(k_termios_t)))
            { rc = -14; break; }
        copy_from_user(&tty->termios, (const void *)arg, sizeof(k_termios_t));
        rc = 0; break;

    case TIOCGPGRP: {
        uint32_t val = tty->fg_pgrp;
        if (!user_ptr_valid(arg, sizeof(uint32_t)))
            { rc = -14; break; }
        copy_to_user((void *)arg, &val, sizeof(uint32_t));
        rc = 0; break;
    }

    case TIOCSPGRP: {
        uint32_t val;
        if (!user_ptr_valid(arg, sizeof(uint32_t)))
            { rc = -14; break; }
        copy_from_user(&val, (const void *)arg, sizeof(uint32_t));
        tty->fg_pgrp = val;
        rc = 0; break;
    }

    case TIOCGWINSZ: {
        uint16_t ws[4];
        if (!user_ptr_valid(arg, sizeof(ws)))
            { rc = -14; break; }
        ws[0] = tty->rows;
        ws[1] = tty->cols;
        ws[2] = 0; /* xpixel */
        ws[3] = 0; /* ypixel */
        copy_to_user((void *)arg, ws, sizeof(ws));
        rc = 0; break;
    }

    case TIOCSWINSZ: {
        uint16_t ws[4];
        if (!user_ptr_valid(arg, sizeof(ws)))
            { rc = -14; break; }
        copy_from_user(ws, (const void *)arg, sizeof(ws));
        tty->rows = ws[0];
        tty->cols = ws[1];
        /* Release lock before signal delivery to avoid lock-ordering issues */
        spin_unlock_irqrestore(&tty_global_lock, fl);
        if (tty->fg_pgrp)
            signal_send_pgrp(tty->fg_pgrp, SIGWINCH);
        return 0;
    }

    default:
        rc = -25; break; /* ENOTTY */
    }
    spin_unlock_irqrestore(&tty_global_lock, fl);
    return rc;
}
