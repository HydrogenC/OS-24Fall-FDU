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
            arch_wfi();
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
    for (u64 q = (u64)icode; q < (u64)eicode; q += PAGE_SIZE) {
        // Map code to EXTMEM
        vmmap(&proc->pgdir, EXTMEM + q - (u64)icode, q, PTE_USER_DATA);
    }
    ASSERT(proc->pgdir.pt);

    struct section *code_section =
            (struct section *)kalloc(sizeof(struct section));
    code_section->begin = EXTMEM;
    code_section->end = EXTMEM + (eicode - icode);
    code_section->flags = ST_DATA;
    code_section->fp = NULL;
    _insert_into_list(&proc->pgdir.section_head, &code_section->stnode);

    proc->ucontext->elr = EXTMEM;
    // Hint enter user mode
    proc->ucontext->spsr = 0;
    start_proc(proc, trap_return, 0);

    while (1)
        yield();
    /* (Final) TODO END */
}

NO_INLINE NO_RETURN void _panic(const char *file, int line)
{
    printk("=====%s:%d PANIC%lld!=====\n", file, line, cpuid());
    panic_flag = true;
    set_cpu_off();
    for (int i = 0; i < NCPU; i++) {
        if (cpus[i].online)
            i--;
    }
    printk("Kernel PANIC invoked at %s:%d. Stopped.\n", file, line);
    arch_stop_cpu();
}