#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <test/test.h>
#include <common/buf.h>
#include <driver/virtio.h>

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
            arch_wfe();
        }
    }
    set_cpu_off();
    arch_stop_cpu();
}

NO_RETURN void kernel_entry()
{
    printk("Hello world! (Core %lld)\n", cpuid());
    // proc_test();
    // vm_test();
    // user_proc_test();
    io_test();

    /* LAB 4 TODO 3 BEGIN */
    Buf buffer;
    buffer.block_no = 0;
    buffer.flags = 0;

    virtio_blk_rw(&buffer);

    u32* lba_part_2 = (u32*)&buffer.data[0x1CE + 0x8];
    u32* numsec_part_2 = (u32*)&buffer.data[0x1CE + 0xC];

    printk("LBA of partition 2 is %u. \n", *lba_part_2);
    printk("Number of sectors of partition 2 is %u. \n", *numsec_part_2);
    /* LAB 4 TODO 3 END */

    while (1)
        yield();
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