#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/rbtree.h>
#include <kernel/cpu.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };

// Caller-saved registers here
typedef struct UserContext {
    // General purpose, x2 ~ x18
    u64 x[18];
    // `另外，为了防止我们不小心把 ELR_EL1 和 SPSR_EL1 的值覆盖掉，我们也需要保存这两个寄存器的值`
    u64 elr, spsr;
} UserContext;

// Save callee-saved registers here
// Reference: https://developer.arm.com/documentation/102374/0101/Procedure-Call-Standard
typedef struct KernelContext {
    // General purpose, x19 ~ x29
    u64 x[11];
    // Link register, for jumping to the code of the process
    u64 lr;
    // Argument 0 and 1
    u64 x0;
    u64 x1;
} KernelContext;

typedef struct __cpu_state{
    struct Proc* thisProc;
    struct Proc* idleProc;
} CpuState;

// embeded data for procs
struct schinfo {
    // Node for round-robin scheduling
    ListNode queueNode;
};

typedef struct Proc {
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct Proc *parent;
    struct schinfo schinfo;
    void *kstack;
    UserContext *ucontext;
    KernelContext *kcontext;
} Proc;

void init_kproc();
void init_proc(Proc *);
Proc *create_proc();
int start_proc(Proc *, void (*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int *exitcode);
