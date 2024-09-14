#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
#include <kernel/printk.h>

// Reference: https://stackoverflow.com/questions/4840410/how-to-align-a-pointer-in-c
#define ALIGN_PTR(addr, size) ((usize)addr + (size - 1)) & (-size);

RefCount kalloc_page_cnt;

extern char end[];
static char *heap_base;

void kinit() {
    init_rc(&kalloc_page_cnt);
    heap_base = ALIGN_PTR(end, PAGE_SIZE);
    printk("Start addr: %llu\n", (usize)heap_base);
}

void* kalloc_page() {
    increment_rc(&kalloc_page_cnt);

    return NULL;
}

void kfree_page(void* p) {
    decrement_rc(&kalloc_page_cnt);
    return;
}

void* kalloc(unsigned long long size) {
    return NULL;
}

void kfree(void* ptr) {
    return;
}
