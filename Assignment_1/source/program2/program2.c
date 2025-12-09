#include <linux/module.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/jiffies.h>
#include <linux/kmod.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/signal.h>

MODULE_LICENSE("GPL");
extern struct filename *getname_kernel(const char *filename);
extern void putname(struct filename *name);
extern pid_t kernel_clone(struct kernel_clone_args *kargs);
extern long kernel_execve(const char *filename, const char *const *argv, const char *const *envp);
extern int kernel_wait(pid_t pid, int *stat);

extern void msleep(unsigned int msecs);

static const char *map_status(int sig)
{
    switch (sig) {
    case SIGHUP:   return "SIGHUP";
    case SIGINT:   return "SIGINT";
    case SIGQUIT:  return "SIGQUIT";
    case SIGILL:   return "SIGILL";
    case SIGTRAP:  return "SIGTRAP";
    case SIGABRT:  return "SIGABRT";
    case SIGBUS:   return "SIGBUS";
    case SIGFPE:   return "SIGFPE";
    case SIGKILL:  return "SIGKILL";
    case SIGUSR1:  return "SIGUSR1";
    case SIGSEGV:  return "SIGSEGV";
    case SIGUSR2:  return "SIGUSR2";
    case SIGPIPE:  return "SIGPIPE";
    case SIGALRM:  return "SIGALRM";
    case SIGTERM:  return "SIGTERM";
    case SIGSTKFLT:return "SIGSTKFLT";
    case SIGCHLD:  return "SIGCHLD";
    case SIGCONT:  return "SIGCONT";
    case SIGSTOP:  return "SIGSTOP";
    case SIGTSTP:  return "SIGTSTP";
    case SIGTTIN:  return "SIGTTIN";
    case SIGTTOU:  return "SIGTTOU";
    case SIGURG:   return "SIGURG";
    case SIGXCPU:  return "SIGXCPU";
    case SIGXFSZ:  return "SIGXFSZ";
    case SIGVTALRM:return "SIGVTALRM";
    case SIGPROF:  return "SIGPROF";
    case SIGWINCH: return "SIGWINCH";
    case SIGIO:    return "SIGIO";
    case SIGPWR:   return "SIGPWR";
    case SIGSYS:   return "SIGSYS";
    default:       return "UNKNOWN";
    }
}

static int my_exec(void)
{
    pid_t pid = current->pid;
    const char *path = "/tmp/test";

    printk("[program2] : The child process has pid = %d\n", pid);
    printk("[program2] : child process\n");

    {
        const char *argv[] = { "test", NULL };
        const char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
        struct filename *fn = getname_kernel(path);
        if (IS_ERR(fn)) {
            printk(KERN_ERR "[program2] : getname_kernel failed\n");
            return PTR_ERR(fn);
        }

        long rc = kernel_execve(fn->name, argv, envp);
        putname(fn);

        if (rc < 0)
            printk(KERN_ERR "[program2] : kernel_execve failed, rc=%ld\n", rc);

        return (int)rc;
    }
}

static int my_fork(void *arg)
{
    int i, status = 0;
    pid_t child_pid;

    printk("[program2] : module_init kthread start\n");
    {
        struct k_sigaction *k_action = &current->sighand->action[0];
        for (i = 0; i < _NSIG; i++) {
            k_action->sa.sa_handler  = SIG_DFL;
            k_action->sa.sa_flags    = 0;
            k_action->sa.sa_restorer = NULL;
            sigemptyset(&k_action->sa.sa_mask);
            k_action++;
        }
    }

    {
        struct kernel_clone_args kargs = {
            .flags        = SIGCHLD,      
            .pidfd        = NULL,
            .child_tid    = NULL,
            .parent_tid   = NULL,
            .exit_signal  = SIGCHLD,
            .stack        = (unsigned long)my_exec, 
            .stack_size   = 0,
            .tls          = 0,
        };

        child_pid = kernel_clone(&kargs);
        if (child_pid < 0) {
            printk(KERN_ERR "[program2] : kernel_clone failed, rc=%d\n", child_pid);
            return child_pid;
        }
    }

    printk("[program2] : This is the parent process, pid = %d\n", current->pid);

    if (kernel_wait(child_pid, &status) < 0) {
        printk(KERN_ERR "[program2] : kernel_wait failed\n");
        return -1;
    }

    if ((status & 0x7f) == 0) {
        int exit_code = (status >> 8) & 0xff;
        printk("[program2] : child process terminated\n");
        printk("[program2] : Normal exit, code = %d\n", exit_code);
    } else if ((status & 0x7f) == 0x7f) {
        int stop_sig = (status >> 8) & 0xff;
        printk("[program2] : get %s signal\n", map_status(stop_sig));
        printk("[program2] : child process stopped, signal = %d\n", stop_sig);
    } else {
        int term_sig = status & 0x7f;
        printk("[program2] : get %s signal\n", map_status(term_sig));
        if (status & 0x80)
            printk("[program2] : Child produced a core dump\n");
        printk("[program2] : child process terminated\n");
        printk("[program2] : The return signal is %d\n", term_sig);
    }
    return 0;
}

static int __init program2_init(void)
{
    struct task_struct *t;

    printk("[program2] : module_init\n");
    printk("[program2] : module_init create kthread start\n");
    t = kthread_run(my_fork, NULL, "my_fork");
    if (IS_ERR(t)) {
        printk(KERN_ERR "[program2] : kthread_run failed, rc=%ld\n", PTR_ERR(t));
        return PTR_ERR(t);
    }

    return 0;
}

static void __exit program2_exit(void)
{
    printk("[program2] : module_exit\n");
}

module_init(program2_init);
module_exit(program2_exit);






