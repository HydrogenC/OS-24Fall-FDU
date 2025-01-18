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

#define ALIGN_UP(addr, size) (((usize)(addr) + (size - 1)) & (-size))
#define ALIGN_DOWN(addr, size) (((usize)(addr)) & (-size))

void init_sections(ListNode *section_head)
{
    /* (Final) TODO BEGIN */
    init_list_node(section_head);
    /* (Final) TODO END */
}

void free_sections(struct pgdir *pd)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pd->lock);
    ListNode *node = pd->section_head.next;
    while (node != &pd->section_head) {
        struct section *section = container_of(node, struct section, stnode);
        ListNode *next = node->next;

        if (section->flags == ST_MMAP_SHARED &&
            (section->prot & 2 /* PROT_WRITE */)) {
            write_back(pd, section->fp, section->begin, section->offset,
                       section->length);
        }

        if (section->fp) {
            file_close(section->fp);
        }

        _detach_from_list(node);
        kfree(section);
        node = next;
    }
    release_spinlock(&pd->lock);
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
    acquire_spinlock(&p->pgdir.lock);
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

    if (!containing_section) {
        printk("(warn) Requested address (%llu) isn't inside a section! \n",
               addr);
        release_spinlock(&p->pgdir.lock);
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
            containing_section->flags & ST_STACK || containing_section->fp) {
            void *new_page = kalloc_page();
            if (!new_page) {
                release_spinlock(&p->pgdir.lock);
                return -1;
            }

            vmmap(pd, page_addr, new_page, PTE_USER_DATA);

            // Read content from file
            if (containing_section->fp) {
                ASSERT((containing_section->flags & ST_FILE) ||
                       (containing_section->flags & ST_MMAP));
                u64 flags = PTE_USER_DATA;
                if ((containing_section->flags & ST_MMAP) &&
                    containing_section->prot == 1 /* PROT_READ */) {
                    flags |= PTE_RO;
                } else if (containing_section->flags == ST_MMAP_PRIVATE) {
                    flags |= PTE_RO;
                }

                map_file(pd, containing_section->fp, containing_section->begin,
                         containing_section->offset, containing_section->length,
                         flags);
            }

            release_spinlock(&p->pgdir.lock);
            return 0;
        } else {
            printk("(warn) translation error not resolvable.\n");
            release_spinlock(&p->pgdir.lock);
            return -1;
        }
    }

    // Permission fault
    if ((dfsc >> 2) == 0x3) {
        // Check `WnR` bit, this fault should be caused by a write command
        ASSERT(iss & 0x40);

        if ((containing_section->flags & ST_MMAP) &&
            containing_section->prot == 1 /* PROT_READ */) {
            printk("(warn) attempting to write readonly mmap.\n");
            release_spinlock(&p->pgdir.lock);
            return -1;
        }

        // Do a COW
        // printk("Doing COW at %llu\n", addr);
        PTEntriesPtr pte = get_pte(pd, addr, false);
        ASSERT(pte != NULL && (*pte & 0x1));
        void *old_page_addr = (void *)P2K(PTE_ADDRESS(*pte));

        // Allocate a new page and copy
        void *new_page = kalloc_page();
        if (!new_page) {
            release_spinlock(&p->pgdir.lock);
            return -1;
        }

        // Copy the contents of the old page
        memcpy(new_page, old_page_addr, PAGE_SIZE);
        // Ref to the old page will be released in vmmap
        vmmap(pd, page_addr, new_page, PTE_USER_DATA);
        release_spinlock(&p->pgdir.lock);
        return 0;
    }

    // Permission fault, address size fault, etc
    printk("Failed to handle page fault, killing proc %d\n", thisproc()->pid);
    release_spinlock(&p->pgdir.lock);
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
            copied->fp = file_dup(section->fp);
            copied->offset = section->offset;
            copied->length = section->length;
        }

        _insert_into_list(to_head, &copied->stnode);
        node = node->next;
    }
    /* (Final) TODO END */
}

int map_file(struct pgdir *pd, File *f, u64 va, usize offset, usize len,
             u64 flags)
{
    usize bytes_read = 0;
    u64 va_pos = va;
    f->off = offset;

    while (bytes_read < len) {
        u64 va_page_base = PAGE_BASE(va_pos);
        PTEntriesPtr pte = get_pte(pd, va_page_base, false);

        char *phys_page = NULL;
        if (!pte || !CHECK_DESCRIPTOR(*pte)) {
            phys_page = kalloc_page();
            memset(phys_page, 0, PAGE_SIZE);
            vmmap(pd, va_page_base, phys_page, flags);
        } else {
            phys_page = (char *)P2K(PTE_ADDRESS(*pte));
        }

        u64 va_offset_in_page = va_pos - va_page_base;
        u32 should_read = MIN(PAGE_SIZE - va_offset_in_page, len - bytes_read);

        u32 read_count =
                file_read(f, phys_page + va_offset_in_page, should_read);

        bytes_read += read_count;
        va_pos += read_count;
        if (read_count != should_read) {
            va_page_base = PAGE_BASE(va_pos);
            va_offset_in_page = va_pos - va_page_base;

            // Fill rest of this page with zero
            if (va_pos % PAGE_SIZE != 0) {
                u64 zero_count = ALIGN_UP(va_pos, PAGE_SIZE) - va_pos;
                memset(phys_page + va_offset_in_page, 0, zero_count);
                va_pos += zero_count;
            }

            ASSERT(va_pos % PAGE_SIZE == 0);
            // Map the rest to shared zero page
            while (va_pos < va + len) {
                /* code */
                vmmap(pd, va_pos, get_zero_page(), PTE_USER_DATA | PTE_RO);
                va_pos += PAGE_SIZE;
            }

            return bytes_read;
        }
    }

    return bytes_read;
}

int write_back(struct pgdir *pd, File *f, u64 va, usize offset, usize len)
{
    usize bytes_written = 0;
    u64 va_pos = va;
    f->off = offset;

    while (bytes_written < len) {
        u64 va_page_base = PAGE_BASE(va_pos);
        PTEntriesPtr pte = get_pte(pd, va_page_base, false);

        if (!pte || !CHECK_DESCRIPTOR(*pte)) {
            // Pages aren't created yet, so there's no modifications, we can safely return
            // printk("(info) pages unmodified, no need to write back\n");
            return 0;
        }

        char *phys_addr = (char *)P2K(PTE_ADDRESS(*pte));

        u64 va_offset_in_page = va_pos - va_page_base;
        u32 write_count =
                MIN(PAGE_SIZE - va_offset_in_page, len - bytes_written);

        if (file_write(f, phys_addr + va_offset_in_page, write_count) !=
            write_count) {
            printk("(warn) write failure when writing back\n");
            return -1;
        }

        bytes_written += write_count;
        va_pos += write_count;
    }

    return bytes_written;
}
