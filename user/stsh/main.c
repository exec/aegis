#include "stsh.h"

/*
 * build_prompt — format "user@aegis:path$ " or "user@aegis:path# ".
 * Replaces HOME prefix with ~.
 */
static void
build_prompt(char *prompt, int len)
{
    const char *user = env_get("USER");
    if (!user) user = "aegis";

    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        strncpy(cwd, "?", sizeof(cwd));

    /* Replace HOME prefix with ~ */
    const char *home = env_get("HOME");
    char display_path[256];
    if (home && home[0] && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(display_path, sizeof(display_path), "~%s",
                 &cwd[strlen(home)]);
    } else {
        strncpy(display_path, cwd, sizeof(display_path));
        display_path[sizeof(display_path) - 1] = '\0';
    }

    char suffix = has_cap_delegate() ? '#' : '$';
    snprintf(prompt, len, "%s@aegis:%s%c ", user, display_path, suffix);
}

int
main(int argc, char **argv, char **envp)
{
    char line[LINE_SIZE];
    char expanded[LINE_SIZE];
    cmd_t cmds[MAX_PIPELINE];
    int last_exit = 0;

    /* Initialize environment */
    env_init(envp);

    /* Ensure PATH is set */
    if (!env_get("PATH"))
        env_set("PATH", "/bin");

    /* Cache capability presence */
    caps_init();

    /* Initialize history (privileged = no disk persist) */
    hist_init(has_cap_delegate());

    /* Handle -c command mode */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        strncpy(line, argv[2], sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';

        env_expand(line, expanded, sizeof(expanded), last_exit);

        int n = parse_pipeline(expanded, cmds, MAX_PIPELINE);
        if (n == 0)
            return 0;

        /* Handle exec builtin */
        if (n == 1 && strcmp(cmds[0].argv[0], "exec") == 0 &&
            cmds[0].argv[1]) {
            char full[256];
            char *prog = cmds[0].argv[1];
            char **exec_argv = &cmds[0].argv[1];
            if (prog[0] != '/') {
                snprintf(full, sizeof(full), "/bin/%s", prog);
                execve(full, exec_argv, env_as_array());
            } else {
                execve(prog, exec_argv, env_as_array());
            }
            perror(prog);
            return 127;
        }

        if (!try_builtin(cmds, n, &last_exit))
            run_pipeline(cmds, n, env_as_array(), &last_exit);
        return last_exit;
    }

    printf("[SHELL] stsh ready\n");

    /* Ignore SIGCHLD to prevent zombie accumulation */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigaction(SIGCHLD, &sa, NULL);
    }

    /* REPL */
    for (;;) {
        char prompt[512];
        build_prompt(prompt, sizeof(prompt));

        int rlen = editor_readline(prompt, line, sizeof(line));
        if (rlen < 0)
            break; /* EOF */
        if (rlen == 0)
            continue; /* empty or Ctrl-C */

        /* Skip blank lines */
        {
            int blank = 1;
            for (int i = 0; line[i]; i++) {
                if (line[i] != ' ' && line[i] != '\t') {
                    blank = 0;
                    break;
                }
            }
            if (blank)
                continue;
        }

        hist_add(line);

        /* Expand environment variables */
        env_expand(line, expanded, sizeof(expanded), last_exit);

        /* Handle semicolons for sequential commands */
        char *rest = expanded;
        while (rest && *rest) {
            /* Find next semicolon (not inside quotes — v1 ignores quoting) */
            char *semi = strchr(rest, ';');
            if (semi)
                *semi = '\0';

            /* Strip leading whitespace */
            while (*rest == ' ' || *rest == '\t') rest++;

            if (*rest) {
                /* Make a mutable copy for parse_pipeline (it modifies in place) */
                char seg[LINE_SIZE];
                strncpy(seg, rest, sizeof(seg) - 1);
                seg[sizeof(seg) - 1] = '\0';

                int n = parse_pipeline(seg, cmds, MAX_PIPELINE);
                if (n > 0) {
                    if (!try_builtin(cmds, n, &last_exit))
                        run_pipeline(cmds, n, env_as_array(), &last_exit);
                }
            }

            rest = semi ? semi + 1 : NULL;
        }
    }

    hist_save();
    return last_exit;
}
