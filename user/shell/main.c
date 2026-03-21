#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_ARGS 32
#define LINE_SIZE 256

static int
tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        if (argc >= max - 1) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void
do_cd(char **argv)
{
    const char *path = argv[1] ? argv[1] : "/";
    if (chdir(path) != 0)
        perror(path);
}

static void
do_help(void)
{
    puts("Builtins: cd [path], exit [n], help");
    puts("Other commands are looked up in /bin/");
}

int main(void)
{
    char  line[LINE_SIZE];
    char *argv[MAX_ARGS];
    char *envp[] = { NULL };

    printf("[SHELL] Aegis shell ready\n");
    for (;;) {
        write(1, "# ", 2);
        if (!fgets(line, sizeof(line), stdin))
            break;

        int argc = tokenize(line, argv, MAX_ARGS);
        if (argc == 0) continue;

        if (strcmp(argv[0], "exit") == 0) {
            int code = argv[1] ? atoi(argv[1]) : 0;
            exit(code);
        }
        if (strcmp(argv[0], "cd") == 0) {
            do_cd(argv);
            continue;
        }
        if (strcmp(argv[0], "help") == 0) {
            do_help();
            continue;
        }

        /* External command: fork + exec */
        pid_t pid = fork();
        if (pid == 0) {
            /* Child */
            char full[64];
            if (argv[0][0] != '/') {
                snprintf(full, sizeof(full), "/bin/%s", argv[0]);
                execve(full, argv, envp);
            } else {
                execve(argv[0], argv, envp);
            }
            fprintf(stderr, "%s: not found\n", argv[0]);
            _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
    }
    return 0;
}
