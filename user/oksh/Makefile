CC      = musl-gcc
CFLAGS  = -static -O2 -s -fno-pie -no-pie -Wl,--build-id=none \
          -DEMACS -DVI -DNO_CURSES \
          -I. -I../../references/oksh
VPATH   = ../../references/oksh

SRCS = alloc.c asprintf.c c_ksh.c c_sh.c c_test.c c_ulimit.c edit.c \
       emacs.c eval.c exec.c expr.c history.c io.c jobs.c lex.c mail.c \
       main.c misc.c path.c shf.c syn.c table.c trap.c tree.c tty.c var.c \
       version.c vi.c confstr.c reallocarray.c siglist.c signame.c \
       strlcat.c strlcpy.c strtonum.c unvis.c vis.c issetugid.c

OBJS = $(SRCS:.c=.o)

oksh.elf: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f *.o oksh.elf
