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

    // Init members
    init_list_node(&p->ptnode);
    init_list_node(&p->children);
    init_sem(&p->childexit, 0);
    init_schinfo(&p->schinfo);
    init_pgdir(&p->pgdir);

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
    p->kcontext->lr = &proc_entry;

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
    wait_sem(&this->childexit);
    printk("Proc{pid=%d} got sem signal, sem val=%d. \n", this->pid, this->childexit.val);

    acquire_spinlock(&proc_lock);
    // Move to first child (this->children is a placeholder)
    child = child->next;
    while (child != &this->children) {
        Proc *child_proc = container_of(child, Proc, ptnode);
        // `is_zombie` waits for sched_lock, so that it can be ensured that `sched` has finished. 
        if (is_zombie(child_proc)) {
            *exitcode = child_proc->exitcode;
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
    printk("WARNING: No zombie child found for pid %d, must be something wrong.\n",
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

    this->exitcode = code;
    acquire_spinlock(&proc_lock);
    
    // Notify listeners of child exit
    printk("CPU %d: Proc with pid %d posted exit sem to parent %d. \n", cpuid(), this->pid, this->parent->pid);
    post_sem(&this->parent->childexit);
    // Free pgdir
    free_pgdir(&this->pgdir);

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

    if(proc->state == UNUSED){
        return -1;
    }

    proc->killed = true;
    activate_proc(proc);
    release_spinlock(&proc_lock);
    printk("Killing proc %d with state %d. \n", proc->pid, proc->state);
    return 0;
}