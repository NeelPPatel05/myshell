#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>

// Configuration constants
#define BUF_SIZE       4096
#define MAX_TOKENS     512
#define MAX_PATH       1024
#define MAX_PIPE_CMDS  16
#define MAX_ARGS       512

// Shell state variables
int last_status      = 0;
int total_commands   = 0;
int use_devnull      = 0;

/* STRUCTS */

typedef enum {
    COND_NONE,
    COND_AND,
    COND_OR
} CondKind;

typedef struct {
    char *args[MAX_ARGS];
    char *infile;
    char *outfile;
    int   is_builtin;
} Command;

typedef struct {
    CondKind cond;
    int      num_cmds;              
    Command  commands[MAX_PIPE_CMDS];
} Job;

/* ---------- Forward declarations ---------- */

int   is_builtin_cmd(const char *name);
int   find_executable(char *name, char *full_path);
char *trim_spaces(char *s);

int   split_line_tokens(char *line, char *tokens[], int max_tokens);
int   parse_job_line(char *line, Job *job);
int   run_job(Job *job);

int   run_single_command(Command *cmd, int in_pipeline);
int   run_pipeline(Job *job);

int   run_builtin(Command *cmd);
int   run_builtin_child(Command *cmd);

void  redirect_builtin_io(const char *infile, const char *outfile,
                          int *saved_in, int *saved_out);
void  restore_builtin_io(int saved_in, int saved_out);


/* HELPERS */

int is_builtin_cmd(const char *name)
{
    if (!name) 
        return 0;
    
    return strcmp(name, "exit")  == 0 ||
           strcmp(name, "die")   == 0 ||
           strcmp(name, "cd")    == 0 ||
           strcmp(name, "pwd")   == 0 ||
           strcmp(name, "which") == 0;
}

int find_executable(char *name, char *full_path)
{
    char *dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    
    for (int i = 0; i < 3; i++) {
        snprintf(full_path, MAX_PATH, "%s/%s", dirs[i], name);
        if (access(full_path, X_OK) == 0)
            return 0;
    }
    return -1;
}

char *trim_spaces(char *s)
{
    while (isspace((unsigned char)*s))
        s++;

    if (*s == 0)
        return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;

    *(end + 1) = '\0';
    return s;
}

/* TOKENIZER */
int split_line_tokens(char *line, char *tokens[], int max_tokens)
{
    int count = 0;
    char *p = line;

    while (*p && count < max_tokens - 1) {
        // Skip whitespace
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        // Comment starts
        if (*p == '#') break;

        // Single-char tokens
        if (*p == '<' || *p == '>' || *p == '|') {
            tokens[count] = strndup(p, 1);
            count++;
            p++;
            continue;
        }

        // Regular word
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '<' && *p != '>' && *p != '|' && *p != '#') {
            p++;
        }
        size_t len = p - start;
        char *word = malloc(len + 1);
        if (!word) {
            perror("malloc");
            exit(1);
        }
        memcpy(word, start, len);
        word[len] = '\0';
        tokens[count++] = word;
    }

    tokens[count] = NULL;
    return count;
}

//PARSING LOGIC

static CondKind get_conditional(char *tokens[], int token_count, int *start_index)
{
    *start_index = 0;

    if (token_count == 0)
        return COND_NONE;

    if (strcmp(tokens[0], "and") == 0) {
        *start_index = 1;
        return COND_AND;
    }
    if (strcmp(tokens[0], "or") == 0) {
        *start_index = 1;
        return COND_OR;
    }

    return COND_NONE;
}

/*
 * Parse redirections + argv from a token segment [start, end).
 * Fills a Command struct.
 */
static int parse_command_tokens(char *tokens[], int start, int end, Command *cmd)
{
    cmd->infile   = NULL;
    cmd->outfile  = NULL;
    memset(cmd->args, 0, sizeof(cmd->args));

    int argc = 0;
    for (int i = start; i < end; ) {
        if (strcmp(tokens[i], "<") == 0) {
            if (i + 1 >= end) {
                fprintf(stderr, "Error: no input file specified\n");
                last_status = 1;
                return -1;
            }
            cmd->infile = tokens[i + 1];
            i += 2;
        } else if (strcmp(tokens[i], ">") == 0) {
            if (i + 1 >= end) {
                fprintf(stderr, "Error: no output file specified\n");
                last_status = 1;
                return -1;
            }
            cmd->outfile = tokens[i + 1];
            i += 2;
        } else {
            if (argc < MAX_ARGS - 1) {
                cmd->args[argc++] = tokens[i++];
            } else {
                fprintf(stderr, "Error: too many arguments\n");
                return -1;
            }
        }
    }

    cmd->args[argc] = NULL;
    if (argc == 0) {
        return -1; // empty command
    }

    cmd->is_builtin = is_builtin_cmd(cmd->args[0]);
    return 0;
}

int parse_job_line(char *line, Job *job)
{
    char *clean = trim_spaces(line);
    if (*clean == '\0') return -1;

    // Tokenize
    char *tokens[MAX_TOKENS];
    int token_count = split_line_tokens(clean, tokens, MAX_TOKENS);
    if (token_count == 0) return -1;

    // Conditional at the front?
    int start_index = 0;
    job->cond = get_conditional(tokens, token_count, &start_index);

    if (job->cond != COND_NONE && total_commands == 0) {
        fprintf(stderr, "Error: conditional commands cannot be the first command\n");
        for (int i = 0; i < token_count; i++) free(tokens[i]);
        return -1;
    }

    int first = start_index;
    int last  = token_count;

    if (first >= last) {
        for (int i = 0; i < token_count; i++) free(tokens[i]);
        return -1;
    }

    // Split by '|' into pipeline segments
    int seg_start_indices[MAX_PIPE_CMDS];
    int seg_end_indices[MAX_PIPE_CMDS];
    int seg_count = 0;

    int seg_start = first;
    for (int i = first; i <= last; i++) {
        if (i == last || strcmp(tokens[i], "|") == 0) {
            if (seg_count >= MAX_PIPE_CMDS) {
                fprintf(stderr, "Error: too many pipeline stages\n");
                for (int k = 0; k < token_count; k++) free(tokens[k]);
                return -1;
            }
            seg_start_indices[seg_count] = seg_start;
            seg_end_indices[seg_count]   = i;
            seg_count++;
            seg_start = i + 1;
        }
    }

    job->num_cmds = seg_count;

    // Parse each segment into a Command
    for (int s = 0; s < seg_count; s++) {
        if (parse_command_tokens(tokens,
                                 seg_start_indices[s],
                                 seg_end_indices[s],
                                 &job->commands[s]) != 0) {
            for (int k = 0; k < token_count; k++) free(tokens[k]);
            return -1;
        }
    }

    return 0;
}

//BUILT IN COMANDS

int run_builtin(Command *cmd)
{
    char **argv = cmd->args;
    int argc = 0;
    while (argv[argc]) argc++;

    // exit
    if (strcmp(argv[0], "exit") == 0) {
        printf("mysh: exiting\n");
        exit(0);
    }

    // die
    if (strcmp(argv[0], "die") == 0) {
        for (int i = 1; i < argc; i++)
            fprintf(stderr, "%s ", argv[i]);
        fprintf(stderr, "\nmysh: terminating with failure\n");
        last_status = 1;
        exit(1);
    }

    // cd
    if (strcmp(argv[0], "cd") == 0) {
        if (argc != 2) {
            fprintf(stderr, "cd: expected one argument\n");
            return 1;
        }
        if (chdir(argv[1]) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }

    // pwd
    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[MAX_PATH];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            return 0;
        } else {
            perror("pwd");
            return 1;
        }
    }

    // which
    if (strcmp(argv[0], "which") == 0) {
        if (argc != 2) {
            fprintf(stderr, "which: expected one program name\n");
            return 1;
        }
        if (is_builtin_cmd(argv[1])) {
            fprintf(stderr, "%s: shell built-in\n", argv[1]);
            return 1;
        }
        char path[MAX_PATH];
        if (find_executable(argv[1], path) == 0) {
            printf("%s\n", path);
            return 0;
        } else {
            fprintf(stderr, "%s: not found\n", argv[1]);
            return 1;
        }
    }

    return 1;
}

//Builtins inside a pipeline: run in child process only.
int run_builtin_child(Command *cmd)
{
    char **argv = cmd->args;
    int argc = 0;
    while (argv[argc]) argc++;

    if (strcmp(argv[0], "exit") == 0) {
        exit(0);
    }

    if (strcmp(argv[0], "die") == 0) {
        for (int i = 1; i < argc; i++)
            fprintf(stderr, "%s ", argv[i]);
        fprintf(stderr, "\n");
        exit(1);
    }

    if (strcmp(argv[0], "cd") == 0) {
        if (argc != 2) {
            fprintf(stderr, "cd: expected one argument\n");
            exit(1);
        }
        if (chdir(argv[1]) != 0) {
            perror("cd");
            exit(1);
        }
        exit(0);
    }

    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[MAX_PATH];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            exit(0);
        } else {
            perror("pwd");
            exit(1);
        }
    }

    if (strcmp(argv[0], "which") == 0) {
        if (argc != 2) {
            fprintf(stderr, "which: expected one program name\n");
            exit(1);
        }
        if (is_builtin_cmd(argv[1])) {
            fprintf(stderr, "%s: shell built-in\n", argv[1]);
            exit(1);
        }
        char path[MAX_PATH];
        if (find_executable(argv[1], path) == 0) {
            printf("%s\n", path);
            exit(0);
        } else {
            fprintf(stderr, "%s: not found\n", argv[1]);
            exit(1);
        }
    }

    exit(1);
}

//REDIRECTION HELPERS
void redirect_builtin_io(const char *infile, const char *outfile,
                         int *saved_in, int *saved_out)
{
    *saved_in  = -1;
    *saved_out = -1;

    if (outfile) {
        int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
        if (fd < 0) {
            perror("open outputFile");
            last_status = 1;
            return;
        }
        *saved_out = dup(STDOUT_FILENO);
        if (*saved_out < 0) {
            perror("dup stdout");
            close(fd);
            last_status = 1;
            return;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror("dup2 stdout");
            close(fd);
            last_status = 1;
            return;
        }
        close(fd);
    }

    if (infile) {
        int fd = open(infile, O_RDONLY);
        if (fd < 0) {
            perror("open inputFile");
            last_status = 1;
            return;
        }
        *saved_in = dup(STDIN_FILENO);
        if (*saved_in < 0) {
            perror("dup stdin");
            close(fd);
            last_status = 1;
            return;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror("dup2 stdin");
            close(fd);
            last_status = 1;
            return;
        }
        close(fd);
    }

    last_status = 0;
}

void restore_builtin_io(int saved_in, int saved_out)
{
    if (saved_out != -1) {
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }
    if (saved_in != -1) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }
}


//EXECUTE
int run_single_command(Command *cmd, int in_pipeline)
{
    // If builtin and not in pipeline, run directly in parent
    if (cmd->is_builtin && !in_pipeline) {
        int saved_in  = -1;
        int saved_out = -1;
        if (cmd->infile || cmd->outfile) {
            redirect_builtin_io(cmd->infile, cmd->outfile,
                                &saved_in, &saved_out);
            if (last_status != 0) {
                return 1;
            }
        }
        int rc = run_builtin(cmd);
        restore_builtin_io(saved_in, saved_out);
        last_status = rc;
        total_commands++;
        return rc;
    }

    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        last_status = 1;
        return 1;
    }

    if (pid == 0) {
        // Child
        if (cmd->outfile) {
            int fd = open(cmd->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
            if (fd < 0) {
                perror("open outputFile");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        if (cmd->infile) {
            int fd = open(cmd->infile, O_RDONLY);
            if (fd < 0) {
                perror("open inputFile");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        } else if (use_devnull && !in_pipeline) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd < 0) {
                perror("open /dev/null");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        if (cmd->is_builtin) {
            run_builtin_child(cmd);
        } else {
            if (!strchr(cmd->args[0], '/')) {
                char path[MAX_PATH];
                if (find_executable(cmd->args[0], path) != 0) {
                    fprintf(stderr, "Command not found: %s\n", cmd->args[0]);
                    exit(EXIT_FAILURE);
                }
                cmd->args[0] = path;
            }
            execv(cmd->args[0], cmd->args);
            perror("execv");
            exit(EXIT_FAILURE);
        }
    }

    // Parent
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        last_status = WEXITSTATUS(status);
    } else {
        last_status = 1;
    }
    total_commands++;
    return last_status;
}

//PIPELINE

int run_pipeline(Job *job)
{
    int n = job->num_cmds;
    int pipes[MAX_PIPE_CMDS - 1][2];

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            last_status = 1;
            return 1;
        }
    }

    pid_t pids[MAX_PIPE_CMDS];

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            last_status = 1;
            return 1;
        }

        if (pid == 0) {
            // Child i

            // stdin
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            } else if (job->commands[i].infile) {
                int fd = open(job->commands[i].infile, O_RDONLY);
                if (fd < 0) {
                    perror("open inputFile");
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            } else if (use_devnull) {
                int fd = open("/dev/null", O_RDONLY);
                if (fd < 0) {
                    perror("open /dev/null");
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            // stdout
            if (i < n - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            } else if (job->commands[i].outfile) {
                int fd = open(job->commands[i].outfile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
                if (fd < 0) {
                    perror("open outputFile");
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            // Close all pipes in child
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            Command *cmd = &job->commands[i];

            if (cmd->is_builtin) {
                run_builtin_child(cmd);
            } else {
                if (!strchr(cmd->args[0], '/')) {
                    char path[MAX_PATH];
                    if (find_executable(cmd->args[0], path) != 0) {
                        fprintf(stderr, "Command not found: %s\n", cmd->args[0]);
                        exit(EXIT_FAILURE);
                    }
                    cmd->args[0] = path;
                }
                execv(cmd->args[0], cmd->args);
                perror("execv");
                exit(EXIT_FAILURE);
            }
        }

        pids[i] = pid;
    }

    // Parent closes pipe fds
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    // Wait for children, track last command's exit code
    int status;
    int final_status = 0;
    for (int i = 0; i < n; i++) {
        waitpid(pids[i], &status, 0);
        if (WIFEXITED(status)) {
            if (i == n - 1) {
                final_status = WEXITSTATUS(status);
            }
        } else {
            if (i == n - 1) {
                final_status = 1;
            }
        }
    }

    last_status = final_status;
    total_commands++;
    return last_status;
}

//Do a full job

int run_job(Job *job)
{
    // Conditional execution
    if (job->cond == COND_AND && total_commands > 0 && last_status != 0) {
        return 0; // skip
    }
    if (job->cond == COND_OR && total_commands > 0 && last_status == 0) {
        return 0; // skip
    }

    if (job->num_cmds == 1) {
        return run_single_command(&job->commands[0], 0);
    } else {
        return run_pipeline(job);
    }
}

//TOP LEVEL MODES

static void run_interactive(void)
{
    char line[BUF_SIZE];

    printf("Welcome to my shell!\n");

    while (1) {
        printf("mysh> ");
        fflush(stdout);

        int bytes_read = read(STDIN_FILENO, line, BUF_SIZE - 1);
        if (bytes_read < 1) {
            printf("mysh: exiting\n");
            break;
        }

        line[bytes_read] = '\0';
        line[strcspn(line, "\n")] = '\0';

        Job job;
        if (parse_job_line(line, &job) == 0) {
            run_job(&job);
        }
    }
}

static void run_script_file(const char *path)
{
    char line[BUF_SIZE];
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("Can't open file");
        exit(EXIT_FAILURE);
    }

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        Job job;
        if (parse_job_line(line, &job) == 0) {
            run_job(&job);
        }
    }

    fclose(f);
}

static void run_batch_stdin(void)
{
    char *buffer = NULL;
    size_t bufsize = 0;

    while (getline(&buffer, &bufsize, stdin) != -1) {
        buffer[strcspn(buffer, "\n")] = '\0';
        Job job;
        if (parse_job_line(buffer, &job) == 0) {
            run_job(&job);
        }
    }

    free(buffer);
}

int main(int argc, char *argv[])
{
    int stdin_is_tty = isatty(fileno(stdin));

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [script]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        // Script mode: reading from file, children inherit that file normally
        use_devnull = 0;
        run_script_file(argv[1]);
    } else if (stdin_is_tty) {
        // Interactive mode
        use_devnull = 0;
        run_interactive();
    } else {
        // Batch mode from non-terminal stdin
        use_devnull = 1;
        run_batch_stdin();
    }

    return 0;
}
