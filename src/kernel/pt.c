#include <kernel/pt.h>
#include <kernel/mem.h>
#include <common/string.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>

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
#define CHECK_DESCRIPTOR(entry) (entry & 0x1)

__attribute__((always_inline)) 
PTEntry construct_table_descriptor(PTEntriesPtr next_level_addr){
    PTEntry descriptor = (PTEntry)next_level_addr;

    // Set flag for table descriptor
    descriptor |= PTE_PAGE;
    return descriptor;
}

__attribute__((always_inline)) 
PTEntry construct_page_descriptor(PTEntriesPtr phys_addr){
    PTEntry descriptor = (PTEntry)phys_addr;

    // Set flag for table descriptor
    descriptor |= PTE_PAGE;
    return descriptor;
}

// Allocate a new page table, and write its address to the parent level
PTEntriesPtr allocate_table(PTEntry* parent_level_pte)
{
    PTEntriesPtr new_page_table = kalloc_page();
    
    // Clear memory with zero
    for (int i = 0; i < N_PTE_PER_TABLE; i++){
        new_page_table[i] &= 0x0;
    }

    // Write physical address to parent level page table if applicable
    if (parent_level_pte) {
        *parent_level_pte = construct_table_descriptor(K2P(new_page_table));
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
        pt_l1 = P2K(PTE_ADDRESS(pt_l0[index_l0]));
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
        pt_l2 = P2K(PTE_ADDRESS(pt_l1[index_l1]));
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
        pt_l3 = P2K(PTE_ADDRESS(pt_l2[index_l2]));
    }

    u64 index_l3 = VA_PART3(va);
    return pt_l3 + index_l3;
}

void init_pgdir(struct pgdir *pgdir)
{
    pgdir->pt = NULL;
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

        PTEntriesPtr pt_l1 = P2K(PTE_ADDRESS(pgdir->pt[i0]));
        for (int i1 = 0; i1 < N_PTE_PER_TABLE; i1++) {
            if (!CHECK_DESCRIPTOR(pt_l1[i1])) {
                continue;
            }

            PTEntriesPtr pt_l2 = P2K(PTE_ADDRESS(pt_l1[i1]));
            for (int i2 = 0; i2 < N_PTE_PER_TABLE; i2++) {
                if (!CHECK_DESCRIPTOR(pt_l2[i2])) {
                    continue;
                }

                PTEntriesPtr pt_l3 = P2K(PTE_ADDRESS(pt_l2[i2]));
                kfree_page(pt_l3);
            }

            kfree_page(pt_l2);
        }

        kfree_page(pt_l1);
    }

    kfree_page(pgdir->pt);
}

void attach_pgdir(struct pgdir *pgdir)
{
    extern PTEntries invalid_pt;
    if (pgdir->pt)
        arch_set_ttbr0(K2P(pgdir->pt));
    else
        arch_set_ttbr0(K2P(&invalid_pt));
}
