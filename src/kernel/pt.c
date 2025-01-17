#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <common/string.h>
#include <kernel/mem.h>
#include <kernel/pt.h>
#include <kernel/paging.h>

/*
Reference: https://docs.kernel.org/arch/arm64/memory.html
VIRTUAL ADDR LAYOUT: 
[0:11]  Offset within page (12 bits, 4096 bytes)
[12:20] L3 Index (9 bits, 512 entries)
[21:29] L2 Index (9 bits, 512 entries)
[30:38] L1 Index (9 bits, 512 entries)
[39:47] L0 Index (9 bits, 512 entries)
*/

// Reference: https://wenboshen.org/posts/2018-09-09-page-table
// Check if the entry is valid (the lowest bit of invalid descriptors is 0)
#define CHECK_DESCRIPTOR(entry) ((entry) & 0x1)
#define VA_STOP 0xFFFFFFFFFFFF

PTEntry construct_table_descriptor(PTEntriesPtr next_level_addr)
{
    PTEntry descriptor = (PTEntry)next_level_addr;

    // Set flag for table descriptor
    descriptor |= PTE_PAGE;
    return descriptor;
}

PTEntry construct_page_descriptor(PTEntriesPtr phys_addr)
{
    PTEntry descriptor = (PTEntry)phys_addr;

    // Set flag for table descriptor
    descriptor |= PTE_PAGE;
    return descriptor;
}

// Allocate a new page table, and write its address to the parent level
PTEntriesPtr allocate_table(PTEntry *parent_level_pte)
{
    PTEntriesPtr new_page_table = kalloc_page();

    // Clear memory with zero
    memset(new_page_table, 0, PAGE_SIZE);

    // Write physical address to parent level page table if applicable
    if (parent_level_pte) {
        *parent_level_pte =
                construct_table_descriptor((PTEntriesPtr)K2P(new_page_table));
    }
    return new_page_table;
}

PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc)
{
    // TODO:
    // Return a pointer to the PTE (Page Table Entry) for virtual address 'va'
    // If the entry not exists (NEEDN'T BE VALID), allocate it if alloc=true, or return NULL if false.
    // THIS ROUTINUE GETS THE PTE, NOT THE PAGE DESCRIBED BY PTE.

    // `pgdir->pt` is kernel address
    PTEntriesPtr pt_l0 = pgdir->pt;
    if (!pt_l0) {
        if (alloc) {
            pt_l0 = pgdir->pt = allocate_table(NULL);
        } else {
            return NULL;
        }
    }

    u64 index_l0 = VA_PART0(va);
    PTEntriesPtr pt_l1;

    if (!CHECK_DESCRIPTOR(pt_l0[index_l0])) {
        if (alloc) {
            pt_l1 = allocate_table(pt_l0 + index_l0);
        } else {
            return NULL;
        }
    } else {
        pt_l1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt_l0[index_l0]));
    }

    u64 index_l1 = VA_PART1(va);
    PTEntriesPtr pt_l2;

    if (!CHECK_DESCRIPTOR(pt_l1[index_l1])) {
        if (alloc) {
            pt_l2 = allocate_table(pt_l1 + index_l1);
        } else {
            return NULL;
        }
    } else {
        pt_l2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt_l1[index_l1]));
    }

    u64 index_l2 = VA_PART2(va);
    PTEntriesPtr pt_l3;

    if (!CHECK_DESCRIPTOR(pt_l2[index_l2])) {
        if (alloc) {
            pt_l3 = allocate_table(pt_l2 + index_l2);
        } else {
            return NULL;
        }
    } else {
        pt_l3 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt_l2[index_l2]));
    }

    u64 index_l3 = VA_PART3(va);
    return pt_l3 + index_l3;
}

void init_pgdir(struct pgdir *pgdir)
{
    init_spinlock(&pgdir->lock);

    // Init root table
    pgdir->pt = kalloc_page();
    memset(pgdir->pt, 0, PAGE_SIZE);
}

void free_pgdir(struct pgdir *pgdir)
{
    // TODO:
    // Free pages used by the page table. If pgdir->pt=NULL, do nothing.
    // DONT FREE PAGES DESCRIBED BY THE PAGE TABLE
    if (!pgdir->pt) {
        return;
    }

    for (int i0 = 0; i0 < N_PTE_PER_TABLE; i0++) {
        if (!CHECK_DESCRIPTOR(pgdir->pt[i0])) {
            continue;
        }

        PTEntriesPtr pt_l1 = (PTEntriesPtr)P2K(PTE_ADDRESS(pgdir->pt[i0]));
        for (int i1 = 0; i1 < N_PTE_PER_TABLE; i1++) {
            if (!CHECK_DESCRIPTOR(pt_l1[i1])) {
                continue;
            }

            PTEntriesPtr pt_l2 = (PTEntriesPtr)P2K(PTE_ADDRESS(pt_l1[i1]));
            for (int i2 = 0; i2 < N_PTE_PER_TABLE; i2++) {
                if (!CHECK_DESCRIPTOR(pt_l2[i2])) {
                    continue;
                }

                void *pt_l3 = (void *)P2K(PTE_ADDRESS(pt_l2[i2]));
                kfree_page(pt_l3);
            }

            kfree_page(pt_l2);
        }

        kfree_page(pt_l1);
    }

    kfree_page(pgdir->pt);
    pgdir->pt = NULL;
}

// Copy page table with COW support accroding to the section defs
void copy_pgdir(struct pgdir *src, struct pgdir *dest)
{
    if (!src->pt) {
        return;
    }

    ListNode *node = src->section_head.next;
    // Look for heap section
    while (node != &src->section_head) {
        struct section *section = container_of(node, struct section, stnode);

        u64 page_base = PAGE_BASE(section->begin);
        while (page_base < section->end) {
            PTEntriesPtr src_pte = get_pte(src, page_base, false);

            // Skip unallocated pages
            if (src_pte && CHECK_DESCRIPTOR(*src_pte)) {
                void *phys_page =
                        share_page((void *)P2K(PTE_ADDRESS(*src_pte)));
                vmmap(dest, page_base, phys_page, PTE_USER_DATA | PTE_RO);

                // Change original pte to readonly
                *src_pte |= PTE_RO;
            }

            page_base += PAGE_SIZE;
        }

        node = node->next;
    }

    arch_tlbi_vmalle1is();
}

void attach_pgdir(struct pgdir *pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}

/**
 * Map virtual address 'va' to the physical address represented by kernel
 * address 'ka' in page directory 'pd', 'flags' is the flags for the page
 * table entry.
 */
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags)
{
    /* (Final) TODO BEGIN */

    // TODO
    // Map virtual address 'va' to the physical address represented by kernel
    // address 'ka' in page directory 'pd', 'flags' is the flags for the page
    // table entry

    ASSERT(va % PAGE_SIZE == 0);
    ASSERT((u64)ka % PAGE_SIZE == 0);

    PTEntriesPtr pte = get_pte(pd, va, true);
    ASSERT(pte != NULL);

    // Free the original page if there is
    if ((*pte) & 0x1) {
        // Kernel address of physical page
        void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
        kfree_page(old_page);
    }

    *pte = K2P(ka) | flags;

    // Flush tlb to avoid strange bugs
    arch_tlbi_vmalle1is();

    /* (Final) TODO END */
}

/*
 * Copy len bytes from p to user address va in page table pgdir.
 * Allocate physical pages if required.
 * Useful when pgdir is not the current page table.
 */
int copyout(struct pgdir *pd, void *va, void *p, usize len)
{
    /* (Final) TODO BEGIN */
    char *source = (char *)p;
    u64 va_offset = (u64)va;

    while (len > 0) {
        u64 va_page_base = PAGE_BASE(va_offset);
        PTEntriesPtr pte = get_pte(pd, va_page_base, true);
        if (pte == NULL) {
            return -1;
        }

        // Allocate page if there isn't one
        if (!CHECK_DESCRIPTOR(*pte)) {
            char *new_page = (char *)kalloc_page();
            if (new_page == NULL) {
                return -1;
            }
            *pte = K2P(new_page) | PTE_USER_DATA;
            arch_tlbi_vmalle1is();
        }

        char *page_addr = (char *)P2K(PTE_ADDRESS(*pte));
        u32 offset_in_page = va_offset - va_page_base;
        usize copy_count = MIN(PAGE_SIZE - offset_in_page, len);

        memcpy(page_addr + offset_in_page, source, copy_count);
        len -= copy_count;
        source += copy_count;
        va_offset += copy_count;
    }

    return 0;
    /* (Final) TODO END */
}
