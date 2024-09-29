#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <common/sem.h>

extern bool panic_flag;
static CpuState cpuStates[NCPU];

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

// Pointer to round-robin queue
static ListNode* rrQueue;
static SpinLock sched_lock;

void init_sched()
{
    init_spinlock(&sched_lock);
    init_list_node(rrQueue);

    for (int i = 0; i < NCPU; i++)
    {
        cpuStates[i].idleProc = kalloc(sizeof(Proc));
        cpuStates[i].idleProc->state =RUNNING;
        cpuStates[i].idleProc->idle = true;

        // Run the idle task before any real tasks are scheduled
        cpuStates[i].thisProc = cpuStates[i].idleProc;
    }
    
}

Proc *thisproc()
{
    // TODO: return the current process
    return cpuStates[cpuid()].thisProc;
}

void init_schinfo(struct schinfo *p)
{
    // TODO: initialize your customized schinfo for every newly-created process
    init_list_node(&p->queueNode);
}

void acquire_sched_lock()
{
    // TODO: acquire the sched_lock if need
    acquire_spinlock(&sched_lock);
}

void release_sched_lock()
{
    // TODO: release the sched_lock if need
    release_spinlock(&sched_lock);
}

bool is_zombie(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == ZOMBIE;
    release_sched_lock();
    return r;
}

bool activate_proc(Proc *p)
{
    // TODO:
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic

    switch (p->state) {
    case RUNNING:
    case RUNNABLE:
        return false;
    case SLEEPING:
    case UNUSED:
        p->state = RUNNABLE;
        return true;
    default:
        PANIC();
        return false;
    }
}

static void update_this_state(enum procstate new_state)
{
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and not [remove it from the sched queue if
    // new_state=SLEEPING/ZOMBIE]
    Proc *this = thisproc();
    this->state = new_state;
}

static Proc *pick_next()
{
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process

    // If panicked, then return to idle state
    if(panic_flag){
        return cpuStates[cpuid()].idleProc;
    }

    // Round-robin
}

static void update_this_proc(Proc *p)
{
    // TODO: you should implement this routinue
    // update thisproc to the choosen process
    cpuStates[cpuid()].thisProc = p;
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        swtch(next->kcontext, &this->kcontext);
    }
    release_sched_lock();
}

u64 proc_entry(void (*entry)(u64), u64 arg)
{
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}
