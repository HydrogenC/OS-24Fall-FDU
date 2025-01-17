#pragma once

#include <aarch64/mmu.h>
#include <common/spinlock.h>
#include <common/list.h>
#include <fs/inode.h>

#define ST_FILE  1                  // File-backed
#define ST_SWAP  (1<<1)             // Unused
#define ST_RO    (1<<2)             // Read-only
#define ST_HEAP  (1<<3)             // Section is heap
#define ST_TEXT  (ST_FILE | ST_RO)  // Section is text
#define ST_DATA  ST_FILE            // Section is data
#define ST_BSS   ST_FILE            // Section is bss
#define ST_STACK (1<<4)             // Section is stack

struct pgdir {
    PTEntriesPtr pt;
    SpinLock lock;
    ListNode section_head;
};

void init_pgdir(struct pgdir *pgdir);
WARN_RESULT PTEntriesPtr get_pte(struct pgdir *pgdir, u64 va, bool alloc);
void free_pgdir(struct pgdir *pgdir);
void copy_pgdir(struct pgdir *src, struct pgdir *dest);
void attach_pgdir(struct pgdir *pgdir);
void vmmap(struct pgdir *pd, u64 va, void *ka, u64 flags);
int copyout(struct pgdir *pd, void *va, void *p, usize len);
int load_uvm(struct pgdir *pd, u64 va, Inode *ip, usize offset, usize len);
