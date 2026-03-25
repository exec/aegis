/* pconfig.h — feature flags for oksh on Aegis (musl x86_64). */
#ifndef PCONFIG_H
#define PCONFIG_H

#define HAVE_ASPRINTF
#define HAVE_REALLOCARRAY
#define HAVE_STRLCAT
#define HAVE_STRLCPY
/* strtonum is NOT in musl; use bundled strtonum.c */
/* #define HAVE_STRTONUM */
#define HAVE_ST_MTIM

/* musl provides these timer macros, so suppress the bundled fallbacks */
#define HAVE_TIMERADD
#define HAVE_TIMERCLEAR
#define HAVE_TIMERSUB

/* __dead is OpenBSD-specific; map to GCC noreturn attribute */
#ifndef __dead
#define __dead __attribute__((__noreturn__))
#endif

/* Not available: HAVE_ISSETUGID HAVE_PLEDGE HAVE_CURSES HAVE_NCURSES */
/* Not available: HAVE_STRAVIS HAVE_STRUNVIS HAVE_SIGLIST HAVE_SIGNAME */
/* Not available: HAVE_SIG_T HAVE_SETRESUID HAVE_SETRESGID            */
/* Not available: HAVE_SRAND_DETERMINISTIC HAVE_CONFSTR               */

#endif
