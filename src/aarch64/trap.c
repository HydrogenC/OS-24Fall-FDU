#include <aarch64/trap.h>
#include <aarch64/intrinsic.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <driver/interrupt.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <kernel/paging.h>

#define SPSR_EL1_DAIF_MASK 0xF


// Reference: https://developer.arm.com/documentation/ddi0601/2024-09/AArch64-Registers/SPSR-EL1--Saved-Program-Status-Register--EL1-
#define EXTRACT_MODE(pstate) (pstate & 0xF)
#define GET_DIAF(pstate) ((pstate >> 5) & 0xF)
#define MODE_FLAG_USER ((u64)0x0)

void trap_global_handler(UserContext *context)
{
    thisproc()->ucontext = context;
    
    u64 esr = arch_get_esr();
    u64 ec = esr >> ESR_EC_SHIFT;
    u64 iss = esr & ESR_ISS_MASK;
    u64 ir = esr & ESR_IR_MASK;

    arch_reset_esr();

    switch (ec) {
    case ESR_EC_UNKNOWN: {
        if (ir) {
            printk("Unknown fault, esr is %llu\n", esr);
            PANIC();
        } else {
            interrupt_global_handler();
        }
    } break;
    case ESR_EC_SVC64: {
        syscall_entry(context);
    } break;
    case ESR_EC_IABORT_EL0:
    case ESR_EC_IABORT_EL1:
    case ESR_EC_DABORT_EL0:
    case ESR_EC_DABORT_EL1: {
        pgfault_handler(iss);
    } break;
    default: {
        printk("Unknown exception %llu, esr=%llu\n", ec, esr);
        PANIC();
    }
    }

    // TODO: stop killed process while returning to user space
    u64 mode_flag = GET_DIAF(context->spsr);
    if (mode_flag == 0x0 && thisproc()->killed) {
        printk("CPU %llu: Trapped called on killed process %d, calling exit. \n",
               cpuid(), thisproc()->pid);
        exit(-1);
    }
}

NO_RETURN void trap_error_handler(u64 type)
{
    printk("Unknown trap type %llu\n", type);
    PANIC();
}