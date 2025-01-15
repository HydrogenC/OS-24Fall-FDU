#pragma once

#include <aarch64/mmu.h>
#include <kernel/proc.h>

struct section {
    u64 flags;
    u64 begin;
    u64 end; // [begin, end)
    ListNode stnode;

    /* The following fields are for the file-backed sections. */

    struct file *fp;
    u64 offset; // Offset in file
    u64 length; // Length of mapped content in file
};

int pgfault_handler(u64 iss);
void init_sections(ListNode *section_head);
void free_sections(struct pgdir *pd);
void copy_sections(ListNode *from_head, ListNode *to_head);
u64 sbrk(i64 size);
