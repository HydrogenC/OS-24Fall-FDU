#include <kernel/mem.h>
#include <kernel/cpu.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <common/rc.h>
#include <kernel/printk.h>
#include <kernel/paging.h>
#include "proc.h"

#define ALIGN_UP_PTR(addr, size) (void *)(((usize)addr + (size - 1)) & (-size))

Proc root_proc;
int pid = 0, pid_limit = 65536;

RefCount proc_count;

// Lock for modification on the proc tree
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

int alloc_pid()
{
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
    p->state = UNUSED;

    // Init members
    init_list_node(&p->ptnode);
    init_list_node(&p->children);
    init_sem(&p->childexit, 0);
    init_schinfo(&p->schinfo);
    init_pgdir(&p->pgdir);
    init_sections(&p->pgdir.section_head);

    p->kstack = kalloc_page();
    p->ucontext = (p->kstack + PAGE_SIZE - sizeof(UserContext));
    p->kcontext = (p->kstack + PAGE_SIZE - sizeof(UserContext) -
                   sizeof(KernelContext));

    ASSERT(p->killed == 0);
    release_spinlock(&proc_lock);
}

Proc *create_proc()
{
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

// Walk the runnable list and output, for debug purpose
void __walk_child_list(ListNode *children)
{
    ListNode *current = children->next;
    if (current == children) {
        printk("No child! \n");
        return;
    }

    do {
        Proc *current_proc = container_of(current, Proc, ptnode);
        printk("Proc{pid=%d, state=%d}, ", current_proc->pid,
               current_proc->state);
        current = current->next;
    } while (current != children);

    printk("\n");
}

void set_parent_to_this(Proc *proc)
{
    // TODO: set the parent of proc to thisproc
    // NOTE: maybe you need to lock the process tree
    // NOTE: it's ensured that the old proc->parent = NULL

    ASSERT(proc->parent == NULL);
    Proc *this = thisproc();

    acquire_spinlock(&proc_lock);
    proc->parent = this;
    _insert_into_list(&this->children, &proc->ptnode);
    release_spinlock(&proc_lock);
}

int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    // TODO:
    // 1. set the parent to root_proc if NULL
    // 2. setup the kcontext to make the proc start with proc_entry(entry, arg)
    // 3. activate the proc and return its pid
    // NOTE: be careful of concurrency

    // printk("Start proc called for Proc{pid=%d}\n", p->pid);
    if (p->parent == NULL) {
        acquire_spinlock(&proc_lock);
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
        release_spinlock(&proc_lock);
    }

    // Set first param of the entry (which is stored in x0)
    p->kcontext->x0 = (u64)entry;
    // Set the second param, same as above
    p->kcontext->x1 = (u64)arg;
    // Set the jump address
    p->kcontext->lr = (u64)&proc_entry;

    increment_rc(&proc_count);
    activate_proc(p);
    return p->pid;
}

void recycle_proc(Proc *proc)
{
    // Dealloc the page
    kfree_page(proc->kstack);

    // Dealloc the proc itself
    kfree(proc);
}

int wait(int *exitcode)
{
    // TODO:
    // 1. return -1 if no children
    // 2. wait for childexit
    // 3. if any child exits, clean it up and return its pid and exitcode
    // NOTE: be careful of concurrency

    Proc *this = thisproc();

    acquire_spinlock(&proc_lock);
    ListNode *child = &this->children;
    // No children
    if (child->next == child) {
        release_spinlock(&proc_lock);
        return -1;
    }
    release_spinlock(&proc_lock);

    // printk("Proc{pid=%d} waiting for children. \n", this->pid);
    if (!wait_sem(&this->childexit)) {
        // Proc killed, return directly
        return -1;
    }
    // printk("Proc{pid=%d} got sem signal, sem val=%d. \n", this->pid, this->childexit.val);

    acquire_spinlock(&proc_lock);
    // Move to first child (this->children is a placeholder)
    child = child->next;
    while (child != &this->children) {
        Proc *child_proc = container_of(child, Proc, ptnode);
        // `is_zombie` waits for sched_lock, so that it can be ensured that `sched` has finished.
        if (is_zombie(child_proc)) {
            if (exitcode) {
                *exitcode = child_proc->exitcode;
            }
            int child_pid = child_proc->pid;

            // Recycle child
            _detach_from_list(&child_proc->ptnode);
            recycle_proc(child_proc);
            release_spinlock(&proc_lock);
            return child_pid;
        }

        child = child->next;
    }

    release_spinlock(&proc_lock);
    printk("(warn) No zombie child found for pid %d, must be something wrong.\n",
           this->pid);
    return -1;
}

NO_RETURN void exit(int code)
{
    // TODO:
    // 1. set the exitcode
    // 2. clean up the resources
    // 3. transfer children to the root_proc, and notify the root_proc if there is zombie
    // 4. sched(ZOMBIE)
    // NOTE: be careful of concurrency

    Proc *this = thisproc();
    decrement_rc(&proc_count);

    // Set exit code
    this->exitcode = code;

    // Release files
    for (u64 i = 0; i < NFILE_PROC; i++) {
        if (this->oftable.files[i]) {
            file_close(this->oftable.files[i]);
        }
    }

    if (this->cwd) {
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.put(&ctx, this->cwd);
        bcache.end_op(&ctx);
        this->cwd = NULL;
    }

    // Free pgdir
    free_sections(&this->pgdir);
    free_pgdir(&this->pgdir);

    acquire_spinlock(&proc_lock);

    // Notify listeners of child exit
    // printk("CPU %lld: Proc with pid %d posted exit sem to parent %d. \n", cpuid(), this->pid, this->parent->pid);
    post_sem(&this->parent->childexit);

    ListNode *start_node = &this->children;
    // Transfer children to root_proc if there's any
    if (start_node->next != start_node) {
        ListNode *child = start_node->next;

        while (child != start_node) {
            Proc *child_proc = container_of(child, Proc, ptnode);
            child = child->next;

            _detach_from_list(&child_proc->ptnode);
            child_proc->parent = &root_proc;
            _insert_into_list(&root_proc.children, &child_proc->ptnode);

            // Notify root_proc to clean up if child is zombie
            if (is_zombie(child_proc)) {
                post_sem(&root_proc.childexit);
            }
        }
    }

    acquire_sched_lock();
    release_spinlock(&proc_lock);
    sched(ZOMBIE);

    printk("Shouldn't reach here, must be something wrong with `sched`. \n");
    PANIC();
}

Proc *find_proc(Proc *root, int pid)
{
    if (root->pid == pid) {
        return root;
    }

    // No children
    if (&root->children == root->children.next) {
        return NULL;
    }

    ListNode *current = root->children.next;
    do {
        Proc *current_proc = container_of(current, Proc, ptnode);

        if (current_proc->pid == pid) {
            return current_proc;
        }

        Proc *child_search = find_proc(current_proc, pid);
        if (child_search != NULL) {
            return child_search;
        }

        current = current->next;
    } while (current != &root->children);

    return NULL;
}

int kill(int pid)
{
    // TODO:
    // Set the killed flag of the proc to true and return 0.
    // Return -1 if the pid is invalid (proc not found).

    acquire_spinlock(&proc_lock);
    Proc *proc = find_proc(&root_proc, pid);

    // Process not found
    if (proc == NULL) {
        release_spinlock(&proc_lock);
        return -1;
    }

    if (proc->state == UNUSED) {
        return -1;
    }

    proc->killed = true;
    alert_proc(proc);
    release_spinlock(&proc_lock);
    // printk("Killing proc %d with state %d. \n", proc->pid, proc->state);
    return 0;
}

/*
 * Create a new process copying p as the parent.
 * Sets up stack to return as if from system call.
 */
void trap_return();

int fork()
{
    /**
     * (Final) TODO BEGIN
     * 
     * 1. Create a new child process.
     * 2. Copy the parent's memory space.
     * 3. Copy the parent's trapframe.
     * 4. Set the parent of the new proc to the parent of the parent.
     * 5. Set the state of the new proc to RUNNABLE.
     * 6. Activate the new proc and return its pid.
     */

    Proc *this = thisproc();
    Proc *new_proc = create_proc();
    ASSERT(new_proc != NULL);

    set_parent_to_this(new_proc);
    new_proc->cwd = inodes.share(this->cwd);

    // Copy page table
    acquire_spinlock(&this->pgdir.lock);
    copy_pgdir(&this->pgdir, &new_proc->pgdir);
    copy_sections(&this->pgdir.section_head, &new_proc->pgdir.section_head);
    release_spinlock(&this->pgdir.lock);

    // Copy trap frame
    *(new_proc->ucontext) = *(this->ucontext);
    // Set return values for child proc
    new_proc->ucontext->x[0] = 0;

    // Copy oftable
    for (u64 i = 0; i < NFILE_PROC; i++) {
        if (this->oftable.files[i]) {
            new_proc->oftable.files[i] = file_dup(this->oftable.files[i]);
        }
    }

    // printk("fork pid=%d\n", new_proc->pid);
    // Start and return pid
    return start_proc(new_proc, trap_return, 0);
    /* (Final) TODO END */
}