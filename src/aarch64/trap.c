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

int pgfault_handler(u64 iss)
{
    Proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;

    // Ensure that `far` is valid
    if ((iss << 10) & 0x1) {
        printk("Invalid FAR, cannot handle. \n");
        return -1;
    }

    u64 addr =
            arch_get_far(); // Attempting to access this address caused the page fault
    // TODO:
    // 1. Find the section struct that contains the faulting address `addr`
    // 2. Check section flags to determine page fault type
    // 3. Handle the page fault accordingly
    // 4. Return to user code or kill the process

    // Walk sections
    ListNode *node = p->pgdir.section_head.next;
    struct section *containing_section = NULL;
    // Look for heap section
    while (node != &p->pgdir.section_head) {
        struct section *section = container_of(node, struct section, stnode);
        // Address within section
        if (section->begin <= addr && section->end > addr) {
            containing_section = section;
            break;
        }

        node = node->next;
    }

    if (containing_section == NULL) {
        printk("Warning: Requested address (%llu) isn't inside a section! \n",
               addr);
        return -1;
    }

    u64 page_addr = PAGE_BASE(addr);

    // Reference: https://developer.arm.com/documentation/ddi0601/2024-09/AArch32-Registers/HSR--Hyp-Syndrome-Register
    // Section: `ISS encoding for Exception from a Data Abort`
    const u64 dfsc = iss & 0x3F;

    // Translation fault
    if ((dfsc >> 2) == 0x1) {
        if (containing_section->flags & ST_HEAP) {
            // Do a lazy allocation
            void *new_page = kalloc_page();
            if (!new_page) {
                return -1;
            }

            vmmap(pd, page_addr, new_page, PTE_USER_DATA);
            return 0;
        } else {
            printk("Translation error triggered out of heap section.\n");
            return -1;
        }
    }

    // Permission fault
    if ((dfsc >> 2) == 0x3) {
        // Check `WnR` bit, this fault should be caused by a write command
        ASSERT(iss & 0x40);

        // Do a COW
        PTEntriesPtr pte = get_pte(pd, addr, false);
        ASSERT(pte != NULL);
        void *old_page_addr = (void *)P2K(PTE_ADDRESS(*pte));

        // Allocate a new page and copy
        void *new_page = kalloc_page();
        if (!new_page) {
            return -1;
        }

        // Copy the contents of the old page
        memcpy(new_page, old_page_addr, PAGE_SIZE);
        vmmap(pd, page_addr, new_page, PTE_USER_DATA);
        return 0;
    }

    // Permission fault, address size fault, etc
    return -1;
}

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
        PANIC();
        break;
    case ESR_EC_DABORT_EL0:
    case ESR_EC_DABORT_EL1: {
        // If failed to handle exception, just kill the process
        if (pgfault_handler(iss) == -1) {
            printk("Failed to handle page fault (esr=%lld), killing proc %d\n",
                   esr, thisproc()->pid);
            ASSERT(kill(thisproc()->pid) == 0);
        }
    } break;
    default: {
        printk("Unknown exception %llu\n", ec);
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