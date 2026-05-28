Neel Patel

# Overview
mysh is a simple shell implemented in C 
It supports executing external programs, running built-in commands, handling pipelines, input/output redirection, and conditional execution.

The shell runs in three modes:
Interactive mode – when launched with no arguments. - ./mysh
Batch mode – when given a file containing commands. - ./mysh hello.sh
Batch mode with no specified file – when input is redirected into the shell (non-TTY). cat hello.sh | ./mysh


# Features Supported
Built-in Commands

### The following commands execute directly within the shell:
cd <dir> – Change directory
pwd – Print working directory
which <cmd> – Find command in PATH or report if it is a built-in
exit – Terminate the shell
die [message…] – Print message and exit with failure
Built-ins also work correctly inside pipelines (executed in child processes when piped).

### External Commands
Any non-built-in command is executed via execv after searching the PATH directories:
/usr/local/bin
/usr/bin
/bin

### Redirection
The shell supports:
cmd > file
cmd < file


### Pipelines
cmd1 | cmd2 | cmd3


### Conditionals
echo ok 
and echo two

cd bad_dir
or echo bad

## Structs
Command - basically just one command stage in the shell
Job - one full line of user input

# Logic
Step 1: tokenize - parse inputs inputted by the user into smaller parts. Separating commands, trimming whitespace
Step 2: Parse - Parse the tokens into their separate jobs. Detect where the output is going
Step 3: Execution - Builtins run in the parent process. Forks and executes external commands. Redirects.


# Test Cases
hello.sh - the simplist possible input file used in the early stages of mysh. Just echos hello


