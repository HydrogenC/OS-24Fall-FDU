#include <aarch64/mmu.h>
#include <common/rc.h>
#include <common/spinlock.h>
#include <driver/memlayout.h>
#include <kernel/mem.h>
#include <kernel/printk.h>

// Reference: https://stackoverflow.com/questions/4840410/how-to-align-a-pointer-in-c
#define ALIGN_UP_PTR(addr, size) (void *)(((usize)addr + (size - 1)) & (-size))
#define ALIGN_DOWN_PTR(addr, size) (void *)(((usize)addr) & (-size))

#define MIN_SIZE 8

RefCount kalloc_page_cnt;

extern char end[];
static char *heap_base;

// Block sizes, in bytes
const int block_sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024 };

typedef struct __page_header {
    struct __page_header *next, *prev;
    short filled_blocks;
    char *free_block;
} page_header;

// List of unallocated pages
static page_header *free_list = NULL;
// List of pages that are already allocated, but still have empty blocks
static page_header *partial_list[8] = { NULL };

void kinit()
{
    init_rc(&kalloc_page_cnt);
    heap_base = ALIGN_UP_PTR(end, PAGE_SIZE);

    // Stop addr in kernel space
    int counter = 0;
    char *kernel_stop = P2K(PHYSTOP);
    for (char *i = heap_base; i + PAGE_SIZE <= kernel_stop; i += PAGE_SIZE) {
        page_header *p_header = (page_header *)i;

        if (free_list) {
            free_list->prev = p_header;
        }
        p_header->next = free_list;
        free_list = p_header;
        counter++;
    }

    printk("Page start addr: %llu, registered pages: %d\n", (usize)heap_base,
           counter);
}

void *kalloc_page()
{
    page_header *p_page = free_list;
    if (!p_page) {
        return NULL;
    }

    free_list = p_page->next;
    if (free_list) {
        free_list->prev = NULL;
    }
    increment_rc(&kalloc_page_cnt);
    return p_page;
}

void kfree_page(void *p)
{
    // Insert into free list
    page_header *p_page = p;
    if (free_list) {
        free_list->prev = p_page;
    }
    p_page->next = free_list;
    free_list = p_page;

    decrement_rc(&kalloc_page_cnt);
    return;
}

// Initialize a page to become a container of blocks of a certain size
void setup_page(page_header *p_page, int tier)
{
    // Insert page into the partial list of the block size
    if (partial_list[tier]) {
        partial_list[tier]->prev = p_page;
    }
    p_page->next = partial_list[tier];
    partial_list[tier] = p_page;

    p_page->free_block = NULL;
    p_page->filled_blocks = 0;

    const u64 block_size = block_sizes[tier];
    char *payload_start = ((char *)p_page) + sizeof(page_header);
    payload_start = ALIGN_UP_PTR(payload_start, 8);

    for (char *i = payload_start; i + block_size < p_page + PAGE_SIZE;
         i += block_size) {
        *((char **)i) = p_page->free_block;
        p_page->free_block = i;
    }
}

// Get full pages out of partial_list
void mark_as_full(page_header *p_page, int tier)
{
    if (p_page->prev) {
        p_page->prev->next = p_page->next;
    } else {
        partial_list[tier] = p_page->next;
    }

    if (p_page->next) {
        p_page->next->prev = p_page->prev;
    }
}

// Get the block size tier of the given size
int get_tier(unsigned long long size)
{
    // Ceil size to power of 2
    int leading_zeros = __builtin_clzll(size - 1);
    size = 0x8000000000000000 >> (leading_zeros - 1);

    // Map size to tier by `tier = log2(size) - 3` (ctz is a fast equivalent to log2)
    int trailing_zeros = __builtin_ctzll(size);
    return MAX(0, trailing_zeros - 3);
}

void *kalloc(unsigned long long size)
{
    int tier = get_tier(size);

    page_header *p_page = partial_list[tier];
    // No empty list
    if (!p_page) {
        p_page = kalloc_page();
        if (!p_page) {
            return NULL;
        }

        setup_page(p_page, tier);
    }

    void *addr = p_page->free_block;
    p_page->free_block = *((char **)(p_page->free_block));

    p_page->filled_blocks++;
    if (!p_page->free_block) {
        mark_as_full(p_page, tier);
    }

    return addr;
}

void kfree(void *ptr)
{
    page_header *p_page = ALIGN_DOWN_PTR(ptr, PAGE_SIZE);

    *((char **)ptr) = p_page->free_block;
    p_page->free_block = ptr;
    p_page->filled_blocks--;

    if (p_page->filled_blocks <= 0) {
        kfree_page(p_page);
    }

    return;
}
