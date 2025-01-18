#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>

define_syscall(gettid)
{
    return thisproc()->pid;
}

define_syscall(set_tid_address, int *tidptr)
{
    (void)tidptr;
    return thisproc()->pid;
}

define_syscall(sigprocmask)
{
    return 0;
}

define_syscall(rt_sigprocmask)
{
    return 0;
}

define_syscall(myyield)
{
    yield();
    return 0;
}

define_syscall(yield)
{
    yield();
    return 0;
}

define_syscall(pstat)
{
    return (u64)left_page_cnt();
}

define_syscall(sbrk, i64 size)
{
    return sbrk(size);
}

define_syscall(clone, int flag, void *childstk)
{
    if (flag != 17) {
        printk("sys_clone: flags other than SIGCHLD are not supported.\n");
        return -1;
    }
    (void)childstk;
    return fork();
}

define_syscall(myexit, int n)
{
    exit(n);
}

define_syscall(exit, int n)
{
    exit(n);
}

define_syscall(exit_group, int n)
{
    exit(n);
}

int execve(const char *path, char *const argv[], char *const envp[]);
define_syscall(execve, const char *p, void *argv, void *envp)
{
    if (!user_strlen(p, 256))
        return -1;
    return execve(p, argv, envp);
}

define_syscall(wait4, int pid, int *wstatus, int options, void *rusage)
{
    if (options != 0 || rusage != 0) {
        printk("sys_wait4: unimplemented. pid %d, wstatus 0x%p, options 0x%x, "
               "rusage 0x%p\n",
               pid, wstatus, options, rusage);
    }
    int code, ret;
    if (pid == -1) {
        ret = wait(&code);
    } else {
        int code;
        do {
            ret = wait(&code);
        } while (ret != pid);
    }

    if (wstatus) {
        *wstatus = code;
    }
    return pid;
}

define_syscall(getpid)
{
    return thisproc()->pid;
}
