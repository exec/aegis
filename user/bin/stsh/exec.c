#include "stsh.h"

static long
sys_setfg(long pid)
{
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_SETFG), "D"(pid)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/*
 * exec_cmd — exec the command. Never returns on success.
 */
static void
exec_cmd(cmd_t *cmd, char **envp)
{
    char full[256];
    if (cmd->argv[0][0] != '/') {
        snprintf(full, sizeof(full), "/bin/%s", cmd->argv[0]);
        execve(full, cmd->argv, envp);
    } else {
        execve(cmd->argv[0], cmd->argv, envp);
    }
    fprintf(stderr, "%s: not found\n", cmd->argv[0]);
    _exit(127);
}

/*
 * run_pipeline — execute an N-stage pipeline.
 *
 * Creates N-1 pipes, forks N children with dup2 redirects, then:
 *   1. Parent closes all pipe fds.
 *   2. Parent waits for all children in order.
 *   3. Sets *last_exit from the last child's exit status.
 */
void
run_pipeline(cmd_t *cmds, int n, char **envp, int *last_exit)
{
    int pipes[MAX_PIPELINE - 1][2];
    int i, j;

    /* Create n-1 pipes */
    for (i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            *last_exit = 1;
            return;
        }
    }

    pid_t pids[MAX_PIPELINE];

    for (i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (j = i; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            int st;
            for (j = 0; j < i; j++)
                waitpid(pids[j], &st, 0);
            *last_exit = 1;
            return;
        }

        if (pid == 0) {
            /* ── Child ── */

            /* Wire stdin from previous pipe */
            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);

            /* Wire stdout to next pipe */
            if (i < n - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            /* < stdin redirect */
            if (cmds[i].stdin_file) {
                int fd = open(cmds[i].stdin_file, O_RDONLY);
                if (fd < 0) {
                    fprintf(stderr, "%s: cannot open\n",
                            cmds[i].stdin_file);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* > stdout redirect */
            if (cmds[i].stdout_file && i == n - 1) {
                int fd = open(cmds[i].stdout_file,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    fprintf(stderr, "%s: cannot open for writing\n",
                            cmds[i].stdout_file);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* 2>&1 */
            if (cmds[i].stderr_to_stdout)
                dup2(STDOUT_FILENO, STDERR_FILENO);

            /* Close all pipe fds in child */
            for (j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            exec_cmd(&cmds[i], envp);
            _exit(127);
        }

        pids[i] = pid;
    }

    /* Parent: close all pipe fds */
    for (j = 0; j < n - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }

    /* Set last stage as foreground */
    sys_setfg((long)pids[n - 1]);

    /* Wait for all children */
    int status;
    for (i = 0; i < n; i++)
        waitpid(pids[i], &status, 0);

    /* Clear foreground */
    sys_setfg(0);

    if (WIFEXITED(status))
        *last_exit = WEXITSTATUS(status);
    else
        *last_exit = 1;
}

/*
 * try_builtin — check if a single command is a builtin. Returns 1 if
 * handled, 0 if not. Updates *last_exit.
 */
int
try_builtin(cmd_t *cmds, int n, int *last_exit)
{
    /* Builtins only for a single command, no stdin redirect */
    if (n != 1 || cmds[0].stdin_file || cmds[0].stderr_to_stdout)
        return 0;

    char **argv = cmds[0].argv;
    if (!argv[0])
        return 0;

    if (strcmp(argv[0], "exit") == 0) {
        hist_save();
        int code = argv[1] ? atoi(argv[1]) : *last_exit;
        exit(code);
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *path = argv[1];
        if (!path) {
            path = env_get("HOME");
            if (!path)
                path = "/";
        }
        if (chdir(path) != 0) {
            perror(path);
            *last_exit = 1;
        } else {
            *last_exit = 0;
        }
        return 1;
    }

    if (strcmp(argv[0], "help") == 0) {
        puts("stsh builtins:");
        puts("  cd [path]       - change directory");
        puts("  exit [n]        - exit shell");
        puts("  export VAR=val  - set environment variable");
        puts("  env             - print environment");
        puts("  caps [pid]      - display capabilities");
        puts("  sandbox ...     - run with restricted caps");
        puts("  help            - this message");
        *last_exit = 0;
        return 1;
    }

    if (strcmp(argv[0], "export") == 0) {
        if (!argv[1]) {
            env_print_all();
        } else {
            char *eq = strchr(argv[1], '=');
            if (eq) {
                *eq = '\0';
                env_set(argv[1], eq + 1);
                *eq = '='; /* restore */
            } else {
                fprintf(stderr, "export: usage: export VAR=value\n");
                *last_exit = 1;
                return 1;
            }
        }
        *last_exit = 0;
        return 1;
    }

    if (strcmp(argv[0], "env") == 0) {
        env_print_all();
        *last_exit = 0;
        return 1;
    }

    if (strcmp(argv[0], "caps") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = caps_builtin(argc, argv);
        return 1;
    }

    if (strcmp(argv[0], "sandbox") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = sandbox_builtin(argc, argv, env_as_array());
        return 1;
    }

    if (strcmp(argv[0], "grant") == 0) {
        int argc = 0;
        while (argv[argc]) argc++;
        *last_exit = grant_builtin(argc, argv);
        return 1;
    }

    return 0;
}
