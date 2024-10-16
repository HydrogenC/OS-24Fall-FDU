#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <common/sem.h>
#include <common/rc.h>
#include "sched.h"

extern bool panic_flag;
extern RefCount proc_count;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

// RB tree of runnable processes
static struct rb_root_ sched_tree = { NULL };
static SpinLock sched_lock;

struct KernelContext idle_kcontext[NCPU];
struct timer sched_timers[NCPU];

static bool __sched_cmp(rb_node lnode, rb_node rnode)
{
    auto lsched = container_of(lnode, struct schinfo, sched_node);
    auto rsched = container_of(rnode, struct schinfo, sched_node);
    if (lsched->timestamp < rsched->timestamp)
        return true;
    if (lsched->timestamp == rsched->timestamp)
        return lnode < rnode;
    return false;
}

void timer_handler(struct timer *timer)
{
    timer->triggered = false;
    // Give up CPU ownership to other procs
    acquire_sched_lock();
    sched(RUNNABLE);
}

void init_sched()
{
    init_spinlock(&sched_lock);

    for (int i = 0; i < NCPU; i++) {
        cpus[i].sched.idle_proc = kalloc(sizeof(Proc));
        cpus[i].sched.idle_proc->state = RUNNING;
        cpus[i].sched.idle_proc->idle = true;
        cpus[i].sched.idle_proc->killed = false;
        // Set pid of idle proc to 0 (for debug purposes)
        cpus[i].sched.idle_proc->pid = 0;
        cpus[i].sched.idle_proc->kcontext = &idle_kcontext[i];

        // Run the idle task before any real tasks are scheduled
        cpus[i].sched.this_proc = cpus[i].sched.idle_proc;

        // Setup CPU timers
        sched_timers[i].triggered = false;
        sched_timers[i].elapse = 100;
        sched_timers[i].handler = timer_handler;
    }
}

Proc *thisproc()
{
    // TODO: return the current process
    return cpus[cpuid()].sched.this_proc;
}

void init_schinfo(struct schinfo *p)
{
    // TODO: initialize your customized schinfo for every newly-created process
    p->sched_node.rb_right = p->sched_node.rb_left = NULL;
    p->timestamp = 0;
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

bool is_unused(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == UNUSED;
    release_sched_lock();
    return r;
}

bool activate_proc(Proc *p)
{
    // TODO:
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic

    // printk("CPU %d: Activating Proc{pid=%d, state=%d}, count=%d\n", cpuid(), p->pid, p->state, proc_count.count);
    acquire_sched_lock();

    switch (p->state) {
    case RUNNING:
    case RUNNABLE:
    case ZOMBIE:
        release_sched_lock();
        return false;
    case SLEEPING:
    case UNUSED:
        p->state = RUNNABLE;
        _rb_insert(&p->schinfo.sched_node, &sched_tree, __sched_cmp);
        release_sched_lock();
        return true;
    }

    PANIC();
    release_sched_lock();
    return false;
}

// This function should be called within a lock-protected region
static void update_this_state(enum procstate new_state)
{
    // TODO: if you use template sched function, you should implement this routinue
    // update the state of current process to new_state, and modify the sched queue if necessary
    Proc *this = thisproc();

    enum procstate prev_state = this->state;
    this->state = new_state;

    /*
    if (this->pid != 0) {
        printk("State of Proc{pid=%d} updated from %d to %d\n", this->pid,
               prev_state, new_state);
    }
    */

    // Idle process doesn't need to go into the queue
    if (this->idle) {
        return;
    }

    if (prev_state != RUNNABLE && this->state == RUNNABLE) {
        _rb_insert(&this->schinfo.sched_node, &sched_tree, __sched_cmp);
    } else if (prev_state == RUNNABLE && this->state != RUNNABLE) {
        _rb_erase(&this->schinfo.sched_node, &sched_tree);
    }
}

static Proc *pick_next()
{
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process

    // If panicked or no task to run, then return to idle state
    if (panic_flag || sched_tree.rb_node == NULL) {
        return cpus[cpuid()].sched.idle_proc;
    }

    // Get first process in tree
    rb_node node = _rb_first(&sched_tree);
    if (node != NULL) {
        return container_of(node, Proc, schinfo.sched_node);
    }

    // Default to idle
    // printk("CPU %d: No runnable proc found, falling back to idle\n", cpuid());
    return cpus[cpuid()].sched.idle_proc;
}

static void reset_timer()
{
    // Cancel previous timer (if exists) and setup new timer
    auto timer = &sched_timers[cpuid()];
    if (!timer->triggered) {
        cancel_cpu_timer(timer);
    }
    set_cpu_timer(timer);
}

static void update_this_proc(Proc *p)
{
    // TODO: you should implement this routinue
    // update thisproc to the choosen process
    cpus[cpuid()].sched.this_proc = p;
    reset_timer();
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    update_this_state(new_state);
    this->schinfo.timestamp = get_timestamp_ms();

    // If process killed and not ZOMBIE, then directly return and get back to `trap_global_handler`
    if (this->killed && new_state != ZOMBIE) {
        release_sched_lock();
        return;
    }

    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);

    // Set state as running as remove from schedule tree
    next->state = RUNNING;
    _rb_erase(&next->schinfo.sched_node, &sched_tree);

    /*
    if (next->pid > 1) {
        printk("CPU %llu: Taking on proc with pid %d as next, time elapsed = %llu. \n",
               cpuid(), next->pid,
               get_timestamp_ms() - next->schinfo.timestamp);
    }
    */

    if (next != this) {
        attach_pgdir(&next->pgdir);
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
