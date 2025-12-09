#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <test_program> [args...]\n", prog);
    fprintf(stderr, "Example: %s ./normal\n", prog);
}

static const char* sig_to_name(int sig) {
    switch (sig) {
        case SIGHUP:    return "SIGHUP";
        case SIGINT:    return "SIGINT";
        case SIGQUIT:   return "SIGQUIT";
        case SIGILL:    return "SIGILL";
        case SIGTRAP:   return "SIGTRAP";
        case SIGABRT:   return "SIGABRT";
        case SIGBUS:    return "SIGBUS";
        case SIGFPE:    return "SIGFPE";
        case SIGKILL:   return "SIGKILL";
        case SIGUSR1:   return "SIGUSR1";
        case SIGSEGV:   return "SIGSEGV";
        case SIGUSR2:   return "SIGUSR2";
        case SIGPIPE:   return "SIGPIPE";
        case SIGALRM:   return "SIGALRM";
        case SIGTERM:   return "SIGTERM";
        case SIGCHLD:   return "SIGCHLD";
        case SIGCONT:   return "SIGCONT";
        case SIGSTOP:   return "SIGSTOP";
        case SIGTSTP:   return "SIGTSTP";
        default:        return "UNKNOWN";
    }
}

int main(int argc, char *argv[]) {
    pid_t pid;
    int status;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    printf("Process start to fork\n");
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        printf("I'm the Child Process, my pid = %d\n", getpid());
        printf("Child process start to execute test program:\n");

        execvp(argv[1], &argv[1]);  
        perror("execvp");         
        _exit(127);
    }

    printf("I'm the Parent Process, my pid = %d\n", getpid());

    while (1) {
        pid_t w = waitpid(pid, &status, WUNTRACED | WCONTINUED);
        if (w == -1) {
            perror("waitpid");
            return EXIT_FAILURE;
        }

        printf("Parent process receives SIGCHLD signal\n");

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            printf("Normal termination with EXIT STATUS = %d\n", code);
            break;
        }

        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("child process get %s signal\n", sig_to_name(sig));
            break;
        }

        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            printf("child process get %s signal\n", sig_to_name(sig));
            break; 
        }

        if (WIFCONTINUED(status)) {
            printf("child process continued\n");
        }
    }

    return EXIT_SUCCESS;
}
