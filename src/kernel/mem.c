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
static SpinLock page_lock, block_lock;

extern char end[];
static char *heap_base;

// Block sizes, in bytes
const int block_sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };

typedef struct __page_header {
    struct __page_header *next, *prev;
    int filled_blocks;
    // u16 id;
    int tier;
    char *free_block;
    // bool allocated;
} page_header;

// List of unallocated pages
static page_header *free_list = NULL;
// List of pages that are already allocated, but still have empty blocks
static page_header *partial_list[8] = { NULL };

void init_pages()
{
    heap_base = ALIGN_UP_PTR(end, PAGE_SIZE);

    // Stop addr in kernel space
    int counter = 0;
    char *kernel_stop = P2K(PHYSTOP);
    for (char *i = heap_base; i + PAGE_SIZE <= kernel_stop; i += PAGE_SIZE) {
        page_header *p_header = (page_header *)i;

        if (free_list) {
            free_list->prev = p_header;
        }
        // p_header->id = counter;
        // p_header->allocated = false;
        p_header->next = free_list;
        free_list = p_header;
        counter++;
    }

    printk("Page start addr: %llu, registered pages: %d\n", (usize)heap_base,
           counter);
    printk("Size of header: %llu\n", sizeof(page_header));
}

void kinit()
{
    init_rc(&kalloc_page_cnt);
    init_spinlock(&page_lock);
    init_spinlock(&block_lock);

    init_pages();
}

void *kalloc_page()
{
    acquire_spinlock(&page_lock);
    page_header *p_page = free_list;
    if (!p_page) {
        return NULL;
    }

    free_list = p_page->next;
    if (free_list) {
        free_list->prev = NULL;
    }
    p_page->next = p_page->prev = NULL;

    increment_rc(&kalloc_page_cnt);
    release_spinlock(&page_lock);

    return p_page;
}

void kfree_page(void *p)
{
    // Insert into free list
    acquire_spinlock(&page_lock);
    page_header *p_page = p;
    if (free_list) {
        free_list->prev = p_page;
    }

    // p_page->allocated = false;
    p_page->filled_blocks = 0;
    p_page->free_block = NULL;
    p_page->prev = NULL;
    p_page->next = free_list;
    free_list = p_page;

    decrement_rc(&kalloc_page_cnt);
    release_spinlock(&page_lock);

    return;
}

#define DEBUG_LINE printk("Line %d run\n", __LINE__)

// Get full pages out of partial_list
void remove_from_list(page_header *p_page)
{
    if (p_page->prev) {
        p_page->prev->next = p_page->next;
    } else {
        partial_list[p_page->tier] = p_page->next;
    }

    if (p_page->next) {
        p_page->next->prev = p_page->prev;
    }

    p_page->prev = p_page->next = NULL;
}

// Add page to list if they get partially-full
void add_to_list(page_header *p_page)
{
    int tier = p_page->tier;
    if (partial_list[tier]) {
        partial_list[tier]->prev = p_page;
    }

    p_page->next = partial_list[tier];
    p_page->prev = NULL;
    partial_list[tier] = p_page;
}

// Debug code, to check if the linked list works properly
void __walk_list(page_header *lk)
{
    page_header *pg = lk, *prev_pg;
    int cnt = 0;
    while (pg != NULL) {
        prev_pg = pg;
        pg = pg->next;
        cnt++;
    }

    printk("Forward walk, found %d pages!\n", cnt);
    cnt = 1;

    pg = prev_pg;
    while (pg != lk) {
        pg = pg->prev;
        cnt++;
    }

    printk("Backward walk, found %d pages!\n", cnt);
}

int __count_free_blocks(char *free_blk_ptr)
{
    int counter = 0;
    while (free_blk_ptr) {
        free_blk_ptr = *((char **)free_blk_ptr);
        counter++;
    }

    return counter;
}

// Initialize a page to become a container of blocks of a certain size
void setup_page(page_header *p_page, int tier)
{
    /*
    if (p_page->allocated) {
        printk("PANIC: page %d re-allcated!\n", p_page->id);
    }
    */

    p_page->tier = tier;
    p_page->free_block = NULL;
    p_page->filled_blocks = 0;
    // p_page->allocated = true;
    p_page->next = p_page->prev = NULL;

    // Insert page into the partial list of the block size
    add_to_list(p_page);

    const u64 block_size = block_sizes[tier];
    char *payload_start = ((char *)p_page) + sizeof(page_header);
    // Align to 8
    payload_start = ALIGN_UP_PTR(payload_start, 8);

    const char *upper_bound = (char *)p_page + PAGE_SIZE;
    for (char *i = payload_start; i + block_size < upper_bound;
         i += block_size) {
        *((char **)i) = p_page->free_block;
        p_page->free_block = i;
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
    if (size == 0) {
        // Cannot allocate zero size
        return NULL;
    }

    if (size > 2048) {
        printk("PANIC: %llu is larger than 2048. \n", size);
        return NULL;
    }

    int tier = get_tier(size);
    acquire_spinlock(&block_lock);

    page_header *p_page = partial_list[tier];
    // No empty list
    if (!p_page) {
        p_page = kalloc_page();
        if (!p_page) {
            printk("PANIC: cannot alloc page for tier %d, used pages: %d, returning NULL\n",
                   tier, kalloc_page_cnt.count);
            return NULL;
        }

        setup_page(p_page, tier);
    }

    if (p_page->tier != tier) {
        printk("PANIC: tier mismatch, wanted %d, given %d\n",
            tier, p_page->tier);
    }

    void *addr = p_page->free_block;
    if (!addr) {
        printk("PANIC: full page in partial list\n");
    }
    p_page->free_block = *((char **)addr);

    p_page->filled_blocks++;
    if (!p_page->free_block) {
        remove_from_list(p_page);
    }

    release_spinlock(&block_lock);
    return addr;
}

void kfree(void *ptr)
{
    if (!ptr) {
        printk("PANIC: freeing NULL pointer\n");
        return;
    }

    acquire_spinlock(&block_lock);
    page_header *p_page = ALIGN_DOWN_PTR(ptr, PAGE_SIZE);

    // The page has empty space again after free, add back to partial list
    if (!p_page->free_block) {
        add_to_list(p_page);
    }

    *((char **)ptr) = p_page->free_block;
    p_page->free_block = ptr;

    p_page->filled_blocks--;
    if (p_page->filled_blocks <= 0) {
        // Remove from partial list, and then free page
        remove_from_list(p_page);
        // p_page->allocated = false;
        kfree_page(p_page);
    }

    release_spinlock(&block_lock);
    return;
}
