#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <common/sem.h>
#include <common/rc.h>

extern bool panic_flag;
extern RefCount proc_count;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

// Pointer to cyclic round-robin queue
static ListNode *runnable_queue = NULL;
static SpinLock sched_lock;

struct KernelContext idle_kcontext[NCPU];

void init_sched()
{
    init_spinlock(&sched_lock);

    for (int i = 0; i < NCPU; i++) {
        cpus[i].sched.idle_proc = kalloc(sizeof(Proc));
        cpus[i].sched.idle_proc->state = RUNNING;
        cpus[i].sched.idle_proc->idle = true;
        // Set pid of idle proc to 0 (for debug purposes)
        cpus[i].sched.idle_proc->pid = 0;
        cpus[i].sched.idle_proc->kcontext = &idle_kcontext[i];

        // Run the idle task before any real tasks are scheduled
        cpus[i].sched.this_proc = cpus[i].sched.idle_proc;
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
    init_list_node(&p->queue_node);
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

// Walk the runnable list and output, for debug purpose
void __walk_runnable_list()
{
    if (runnable_queue == NULL) {
        printk("Empty list! \n");
        return;
    }

    ListNode *current = runnable_queue;
    do {
        Proc *current_proc = container_of(current, Proc, schinfo.queue_node);
        printk("Proc{pid=%d}->", current_proc->pid);
        current = current->next;
    } while (current != runnable_queue);

    printk("\n");
}

bool activate_proc(Proc *p)
{
    // TODO:
    // if the proc->state is RUNNING/RUNNABLE, do nothing
    // if the proc->state if SLEEPING/UNUSED, set the process state to RUNNABLE and add it to the sched queue
    // else: panic

    // printk("CPU %d: Activating Proc{pid=%d, state=%d}, count=%d\n", cpuid(), p->pid, p->state, proc_count.count);

    acquire_sched_lock();
    LINE_PROBE;
    switch (p->state) {
    case RUNNING:
    case RUNNABLE:
        release_sched_lock();
        return false;
    case SLEEPING:
    case UNUSED:
        p->state = RUNNABLE;
        runnable_queue =
                insert_into_list(runnable_queue, &p->schinfo.queue_node);
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

    if ((prev_state != RUNNING && prev_state != RUNNABLE) &&
        (this->state == RUNNABLE || this->state == RUNNING)) {
        runnable_queue =
                insert_into_list(runnable_queue, &this->schinfo.queue_node);
    } else if ((prev_state == RUNNING || prev_state == RUNNABLE) &&
               (this->state != RUNNABLE && this->state != RUNNING)) {
        // Transfer list head to next
        if (runnable_queue == &this->schinfo.queue_node) {
            runnable_queue = this->schinfo.queue_node.next;
        }
        detach_from_list(&this->schinfo.queue_node);
    }
}

static Proc *pick_next()
{
    // TODO: if using template sched function, you should implement this routinue
    // choose the next process to run, and return idle if no runnable process

    // If panicked or no task to run, then return to idle state
    if (panic_flag || runnable_queue == NULL) {
        return cpus[cpuid()].sched.idle_proc;
    }

    // Round-robin
    ListNode *rr_node = runnable_queue;
    do {
        Proc *current_proc = container_of(rr_node, Proc, schinfo.queue_node);
        if (current_proc->state == RUNNABLE) {
            // Start from next process next time
            runnable_queue = rr_node->next;
            return current_proc;
        }

        rr_node = rr_node->next;
    } while (rr_node != runnable_queue);

    // Default to idle
    return cpus[cpuid()].sched.idle_proc;
}

static void update_this_proc(Proc *p)
{
    // TODO: you should implement this routinue
    // update thisproc to the choosen process
    cpus[cpuid()].sched.this_proc = p;
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

    if ((this->pid != 0 || next->pid != 0) && this->pid != next->pid ) {
        printk("CPU %d: Current Proc{pid=%d}, new state=%d, picking Proc{pid=%d, state=%d} as next\n",
               cpuid(), this->pid, new_state, next->pid, next->state);
    }
    
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
