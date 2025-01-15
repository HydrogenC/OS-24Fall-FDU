#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/string.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/pt.h>
#include <kernel/sched.h>

void init_sections(ListNode *section_head)
{
    /* (Final) TODO BEGIN */
    init_list_node(section_head);
    /* (Final) TODO END */
}

void free_sections(struct pgdir *pd)
{
    /* (Final) TODO BEGIN */

    ListNode *node = pd->section_head.next;
    while (node != &pd->section_head) {
        struct section *section = container_of(node, struct section, stnode);
        ListNode *next = node->next;

        detach_from_list(&pd->lock, node);
        kfree(section);
        node = next;
    }

    /* (Final) TODO END */
}

u64 sbrk(i64 size)
{
    /**
     * (Final) TODO BEGIN 
     * 
     * Increase the heap size of current process by `size`.
     * If `size` is negative, decrease heap size. `size` must
     * be a multiple of PAGE_SIZE.
     * 
     * Return the previous heap_end.
     */

    Proc *this = thisproc();

    ListNode *node = this->pgdir.section_head.next;
    struct section *heap_section = NULL;
    // Look for heap section
    while (node != &this->pgdir.section_head) {
        struct section *section = container_of(node, struct section, stnode);
        // This section is heap
        if (section->flags & ST_HEAP) {
            heap_section = section;
            break;
        }

        node = node->next;
    }

    if (heap_section == NULL) {
        printk("(warn) proc %d has no heap section\n", this->pid);
        return -1;
    }

    if (heap_section->end + size < heap_section->begin) {
        printk("(warn) invalid heap shrinking size\n");
        return -1;
    }

    u64 original_end = heap_section->end;
    heap_section->end += size;

    if (size < 0) {
        // Free pages that are no longer used

        // Next page to the last page within heap after shrink
        u64 free_pages_start = PAGE_BASE((heap_section->end + (PAGE_SIZE - 1)));
        // Last page within heap before shrink, minus one since `end` is exclusive
        u64 free_pages_end = PAGE_BASE((original_end - 1));
        for (u64 page_addr = free_pages_start; page_addr <= free_pages_end;
             page_addr += PAGE_SIZE) {
            PTEntriesPtr pte = get_pte(&this->pgdir, page_addr, false);
            if (pte && (*pte) & 0x1) {
                void *physical_page = (void *)P2K(PTE_ADDRESS(*pte));
                kfree_page(physical_page);
                *pte = 0;
            }
        }

        // Flush tlb to avoid strange bugs
        arch_tlbi_vmalle1is();
    }

    return original_end;
    /* (Final) TODO END */
}

int pgfault_handler(u64 iss)
{
    Proc *p = thisproc();
    struct pgdir *pd = &p->pgdir;
    u64 addr =
            arch_get_far(); // Attempting to access this address caused the page fault

    /** 
     * (Final) TODO BEGIN
     * 
     * 1. Find the section struct which contains the faulting address `addr`.
     * 2. Check section flags to determine page fault type.
     * 3. Handle the page fault accordingly.
     * 4. Return to user code or kill the process.
     */

    // Ensure that `far` is valid
    if ((iss << 10) & 0x1) {
        printk("ERROR: Invalid FAR, cannot handle. \n");
        return -1;
    }

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
        printk("(warn) Requested address (%llu) isn't inside a section! \n",
               addr);
        return -1;
    }

    u64 page_addr = PAGE_BASE(addr);

    // Reference: https://developer.arm.com/documentation/ddi0601/2024-09/AArch32-Registers/HSR--Hyp-Syndrome-Register
    // Section: `ISS encoding for Exception from a Data Abort`
    const u64 dfsc = iss & 0x3F;

    // Translation fault
    if ((dfsc >> 2) == 0x1) {
        // For lazy-allocated or file-backed sections, allocate physical page
        if (containing_section->flags & ST_HEAP ||
            containing_section->fp != NULL) {
            void *new_page = kalloc_page();
            if (!new_page) {
                return -1;
            }

            vmmap(pd, page_addr, new_page, PTE_USER_DATA);

            // Read content from file
            if (containing_section->fp != NULL) {
                inodes.lock(containing_section->fp->ip);

                u64 offset_in_section = page_addr - containing_section->begin;
                inodes.read(containing_section->fp->ip, new_page,
                            containing_section->offset + offset_in_section,
                            MIN(PAGE_SIZE, containing_section->length -
                                                   offset_in_section));
                inodes.unlock(containing_section->fp->ip);
            }

            return 0;
        } else {
            printk("(warn) Translation error not resolvable.\n");
            return -1;
        }
    }

    // Permission fault
    if ((dfsc >> 2) == 0x3) {
        // Check `WnR` bit, this fault should be caused by a write command
        ASSERT(iss & 0x40);

        // Do a COW
        PTEntriesPtr pte = get_pte(pd, addr, false);
        ASSERT(pte != NULL && (*pte & 0x1));
        void *old_page_addr = (void *)P2K(PTE_ADDRESS(*pte));
        // printk("Doing COW on %llu\n", old_page_addr);

        // Allocate a new page and copy
        void *new_page = kalloc_page();
        if (!new_page) {
            return -1;
        }

        // Copy the contents of the old page
        memcpy(new_page, old_page_addr, PAGE_SIZE);
        // Ref to the old page will be released in vmmap
        vmmap(pd, page_addr, new_page, PTE_USER_DATA);
        return 0;
    }

    // Permission fault, address size fault, etc
    printk("Failed to handle page fault, killing proc %d\n", thisproc()->pid);
    return -1;

    /* (Final) TODO END */
}

void copy_sections(ListNode *from_head, ListNode *to_head)
{
    /* (Final) TODO BEGIN */
    ListNode *node = from_head->next;
    while (node != from_head) {
        struct section *section = container_of(node, struct section, stnode);

        struct section *copied = kalloc(sizeof(struct section));
        copied->begin = section->begin;
        copied->end = section->end;
        copied->flags = section->flags;
        copied->fp = NULL;
        if (section->fp) {
            copied->fp = section->fp;
            copied->offset = section->offset;
            copied->length = section->length;
        }

        _insert_into_list(to_head, &copied->stnode);
        node = node->next;
    }
    /* (Final) TODO END */
}
