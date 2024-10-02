#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/rbtree.h>
#include <kernel/cpu.h>

#define LINE_PROBE printk("CPU %d: File %s, Line %d\n", cpuid(), __FILE__, __LINE__)

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };

// Reference: https://github.com/rcore-os/trapframe-rs/blob/master/src/arch/aarch64/mod.rs
typedef struct UserContext {
    // Reserved for user mode traps, not used now
    u64 tpidr, sp;
    // Special registers
    u64 spsr, elr;
    // x30, reserve 8 bytes for alignment
    u64 lr, __reserved;
    // General purpose registers, x0 ~ x29
    u64 x[30];
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

// embeded data for procs
struct schinfo {
    // Node for round-robin scheduling
    ListNode queue_node;
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
