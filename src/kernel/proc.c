#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <common/rc.h>
#include <kernel/printk.h>

Proc root_proc;
int pid = 0, pid_limit = 65536;

static RefCount proc_count;

// Lock for proc list
static SpinLock proc_lock;

void kernel_entry();
void proc_entry(void (*entry)(u64), u64 arg);

// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc()
{
    // TODO:
    // 1. init global resources (e.g. locks, semaphores)
    // 2. init the root_proc (finished)

    init_rc(&proc_count);
    init_spinlock(&proc_lock);

    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

int alloc_pid(){
    int _pid = ++pid;
    return _pid;
}

void init_proc(Proc *p)
{
    // TODO:
    // setup the Proc with kstack and pid allocated
    // NOTE: be careful of concurrency

    // Clear memory to avoid unexpected values
    memset(p, 0, sizeof(Proc));
    acquire_spinlock(&proc_lock);

    // Allocate pid
    p->pid = alloc_pid();
    
    // Init members
    init_list_node(&p->ptnode);
    init_list_node(&p->children);
    init_sem(&p->childexit, 0);
    init_schinfo(&p->schinfo);

    p->kstack = kalloc_page();
    p->kcontext = (p->kstack + PAGE_SIZE - sizeof(KernelContext));
    p->ucontext = (p->kstack + PAGE_SIZE - sizeof(KernelContext) - sizeof(UserContext));

    release_spinlock(&proc_lock);
}

Proc *create_proc()
{
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc *proc)
{
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL

    ASSERT(proc->parent == NULL);
    Proc* this = thisproc();

    acquire_spinlock(&proc_lock);
    proc->parent = this;
    insert_into_list(&this->children, &proc->ptnode);
    release_spinlock(&proc_lock);
}

int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    // TODO:
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency

    printk("Start proc called for Proc{pid=%d}\n", p->pid);
    if(p->parent == NULL){
        acquire_spinlock(&proc_lock);
        p->parent = &root_proc;
        insert_into_list(&root_proc.children, &p->ptnode);
        release_spinlock(&proc_lock);
    }

    // Set first param of the entry (which is stored in x0)
    p->kcontext->x0 = (u64)entry;
    // Set the second param, same as above
    p->kcontext->x1 = (u64)arg;
    // Set the jump address
    p->kcontext->lr = &proc_entry;

    increment_rc(&proc_count);
    activate_proc(p);
    return p->pid;
}

int wait(int *exitcode)
{
    // TODO:
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency

    Proc* this = thisproc();

    ListNode* child = &this->children;
    // No children
    if(child->next == child){
        return -1;
    }

    wait_sem(&this->childexit);
    
    // Move to first child (this->children is a placeholder)
    child = child->next;
    while (child != &this->children)
    {
        Proc* proc = container_of(child, Proc, ptnode);
        if(proc->state == ZOMBIE){
            *exitcode = proc->exitcode;
            return proc->pid;
        }

        child = child->next;
    }
}

NO_RETURN void exit(int code)
{
    // TODO:
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    // 4. sched(ZOMBIE)
    // NOTE: be careful of concurrency

    Proc* this = thisproc();
    acquire_spinlock(&proc_lock);

    this->exitcode = code;
    // Notify listeners of child exit
    post_sem(&(this->parent->childexit));
    decrement_rc(&proc_count);

    ListNode* child = &this->children;

    // Transfer children to root_proc if there's any
    if(child->next != child){
        child = child->next;
        while (child != &this->children)
        {
            Proc* proc = container_of(child, Proc, ptnode);
            proc->parent = &root_proc;
            insert_into_list(&root_proc.children, &proc->ptnode);

            child = child->next;
        }
    }

    sched(ZOMBIE);
}
