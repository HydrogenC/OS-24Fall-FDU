#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <test/test.h>
#include <aarch64/intrinsic.h>
#include <kernel/paging.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"

void init_syscall()
{
    for (u64 *p = (u64 *)&early_init; p < (u64 *)&rest_init; p++)
        ((void (*)()) * p)();
}

void *syscall_table[NR_SYSCALL] = {
    [0 ... NR_SYSCALL - 1] = NULL,
    [SYS_myreport] = (void *)syscall_myreport,
};

void syscall_entry(UserContext *context)
{
    // TODO
    // Invoke syscall_table[id] with args and set the return value.
    // id is stored in x8. args are stored in x0-x5. return value is stored in x0.
    // be sure to check the range of id. if id >= NR_SYSCALL, panic.
    u64 id = context->x[8];

    if (id >= NR_SYSCALL) {
        PANIC();
    }

    // Get pointer to function
    u64 (*func)(u64, u64, u64, u64, u64, u64) = syscall_table[id];
    u64 ret = -1;
    if (!func) {
        printk("(warn) invalid syscall id %llu\n", id);
    } else {
        ret = func(context->x[0], context->x[1], context->x[2], context->x[3],
                   context->x[4], context->x[5]);
    }

    // Store return value in x0
    context->x[0] = ret;
}

/** 
 * Check if the virtual address [start,start+size) is READABLE by the current
 * user process.
 */
bool user_readable(const void *start, usize size)
{
    /* (Final) TODO BEGIN */
    Proc *this = thisproc();

    ListNode *node = this->pgdir.section_head.next;
    while (node != &this->pgdir.section_head) {
        struct section *section = container_of(node, struct section, stnode);
        // Search for a section that fully encloses the address range (forbid striding across multiple sections)
        if (section->begin <= (u64)start && section->end >= (u64)start + size) {
            // Sections are all readable
            return true;
        }

        node = node->next;
    }

    // No section corresponds to the address given
    return false;
    /* (Final) TODO END */
}

/**
 * Check if the virtual address [start,start+size) is READABLE & WRITEABLE by
 * the current user process.
 */
bool user_writeable(const void *start, usize size)
{
    /* (Final) TODO Begin */
    Proc *this = thisproc();

    ListNode *node = this->pgdir.section_head.next;
    while (node != &this->pgdir.section_head) {
        struct section *section = container_of(node, struct section, stnode);
        // Search for a section that fully encloses the address range (forbid striding across multiple sections)
        if (section->begin <= (u64)start && section->end >= (u64)start + size) {
            // Only of section is writable
            return (section->flags & ST_RO) == 0;
        }

        node = node->next;
    }

    // No section corresponds to the address given
    return false;
    /* (Final) TODO End */
}

/** 
 * Get the length of a string including tailing '\0' in the memory space of
 * current user process return 0 if the length exceeds maxlen or the string is
 * not readable by the current user process.
 */
usize user_strlen(const char *str, usize maxlen)
{
    for (usize i = 0; i < maxlen; i++) {
        if (user_readable(&str[i], 1)) {
            if (str[i] == 0)
                return i + 1;
        } else
            return 0;
    }
    return 0;
}