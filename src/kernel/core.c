#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <kernel/paging.h>
#include <test/test.h>
#include <common/buf.h>
#include <driver/virtio.h>
#include <driver/memlayout.h>

volatile bool panic_flag;

NO_RETURN void idle_entry()
{
    set_cpu_on();
    while (1) {
        yield();
        if (panic_flag)
            break;
        arch_with_trap
        {
            // Note: when using `arch_wfi`, there are conditions where virtio interruptions doesn't
            // trigger virtio interruption handler, especially when there are concurrent requests. 
            // However, using `arch_wfe` instead resolves this issue. 
            // It's kinda weird, but I don't know how to fix this, so I applied this workaround. 
            arch_wfe();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

void trap_return();

NO_RETURN void kernel_entry()
{
    init_filesystem();

    printk("Hello world! (Core %lld)\n", cpuid());
    // proc_test();
    // vm_test();
    // user_proc_test();
    // io_test();
    // paging_test();

    /**
     * (Final) TODO BEGIN 
     * 
     * Map init.S to user space and trap_return to run icode.
     */

    extern char icode[], eicode[];
    Proc *proc = create_proc();

    u64 icode_page = (u64)PAGE_BASE(icode);
    for (u64 q = icode_page; q < (u64)eicode; q += PAGE_SIZE) {
        // Map code to EXTMEM
        vmmap(&proc->pgdir, EXTMEM + q - icode_page, (void *)q, PTE_USER_DATA);
    }
    ASSERT(proc->pgdir.pt);

    struct section *code_section =
            (struct section *)kalloc(sizeof(struct section));
    code_section->begin = EXTMEM + (u64)(icode - icode_page);
    code_section->end = code_section->begin + (eicode - icode);
    code_section->flags = 0;
    _insert_into_list(&proc->pgdir.section_head, &code_section->stnode);

    proc->cwd = inodes.share(inodes.root);

    proc->ucontext->elr = code_section->begin;
    // Put stack pointer at max address
    proc->ucontext->sp = PHYSTOP;
    // Hint enter user mode
    proc->ucontext->spsr = 0;
    start_proc(proc, trap_return, 0);

    // An infinite loop
    while (true) {
        int code;
        int pid = wait(&code);
        ASSERT(pid != 0);
    }

    /* (Final) TODO END */
}

NO_INLINE NO_RETURN void _panic(const char *file, int line)
{
    printk("=====%s:%d PANIC%lld!=====\n", file, line, cpuid());
    panic_flag = true;
    // Block further outputs
    while (true)
        ;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}