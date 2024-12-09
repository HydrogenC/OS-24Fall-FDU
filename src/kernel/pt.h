#pragma once

#include <aarch64/mmu.h>
#include <common/spinlock.h>
#include <common/list.h>

struct section {
    u64 flags;
    u64 begin, end; // [begin, end)
    ListNode stnode;
};

struct pgdir {
    PTEntriesPtr pt;
    SpinLock lock;
    ListNode section_head;
};

void init_pgdir(struct pgdir *pgdir);
WARN_RESULT PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc);
void free_pgdir(struct pgdir *pgdir);
void attach_pgdir(struct pgdir *pgdir);
