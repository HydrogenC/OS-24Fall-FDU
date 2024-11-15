#include <common/bitmap.h>
#include <common/string.h>
#include <fs/cache.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <common/rc.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block device and super block to use.
            Correspondingly, you should NEVER use global instance of
            them, e.g. `get_super_block`, `block_device`

    @see init_bcache
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block device.
 */
static const BlockDevice *device;

/**
    @brief global lock for block cache.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory block.

    We use a linked list to manage all allocated cached blocks.

    You can implement your own data structure if you like better performance.

    @see Block
 */
static ListNode head;

static LogHeader header; // in-memory copy of log header block.

static RefCount num_cached_blocks;

/**
    @brief a struct to maintain other logging states.
    
    You may wonder where we store some states, e.g.
    
    * how many atomic operations are running?
    * are we checkpointing?
    * how to notify `end_op` that a checkpoint is done?

    Put them here!

    @see cache_begin_op, cache_end_op, cache_sync
 */
struct {
    /* your fields here */
} log;

// read the content from disk.
static INLINE void device_read(Block *block)
{
    device->read(block->block_no, block->data);
}

// write the content back to disk.
static INLINE void device_write(Block *block)
{
    device->write(block->block_no, block->data);
}

// read log header from disk.
static INLINE void read_header()
{
    device->read(sblock->log_start, (u8 *)&header);
}

// write log header back to disk.
static INLINE void write_header()
{
    device->write(sblock->log_start, (u8 *)&header);
}

// initialize a block struct.
static void init_block(Block *block)
{
    block->block_no = 0;
    init_list_node(&block->node);
    block->acquired = false;
    block->pinned = false;

    init_sleeplock(&block->lock);
    block->valid = false;
    memset(block->data, 0, sizeof(block->data));
}

// see `cache.h`.
static usize get_num_cached_blocks()
{
    // TODO
    return num_cached_blocks.count;
}

// Walk the cache list, for debug purpose
void __walk_cache_list()
{
    ListNode *node = head.next;
    while (node != &head) {
        Block *current_blk = container_of(node, Block, node);
        printk("Block{no=%llu}->", current_blk->block_no);
        node = node->next;
    }
    printk("\n");
}

// Evict one cache block, typically the last element in list
static int cache_evict() {
    ListNode *node = head.prev;

    acquire_spinlock(&lock);
    // Inverse traverse, find the cache that is used least recently (LRU)
    while (node != &head) {
        Block *current_blk = container_of(node, Block, node);
        // Skip pinned blocks
        if (!current_blk->pinned) {
            printk("Evicting block No. %llu\n", current_blk->block_no);
            _detach_from_list(&current_blk->node);
            decrement_rc(&num_cached_blocks);
            break;
        }

        node = node->prev;
    }
    release_spinlock(&lock);
}

// see `cache.h`.
static Block *cache_acquire(usize block_no)
{
    // TODO
    ListNode *node = head.next;
    Block *blk = NULL;

    acquire_spinlock(&lock);
    while (node != &head) {
        Block *current_blk = container_of(node, Block, node);
        if (current_blk->block_no == block_no) {
            printk("Found block No. %llu\n", current_blk->block_no);
            blk = current_blk;

            // Move node to front of the list, so that the list is ordered by access time (LRU)
            _detach_from_list(&current_blk->node);
            _insert_into_list(&head, &current_blk->node);
            break;
        }

        node = node->next;
    }
    release_spinlock(&lock);

    // Cache block not found, read from disk
    if (!blk) {
        if(get_num_cached_blocks() >= EVICTION_THRESHOLD){
            cache_evict();
        }

        printk("Initing block No. %llu\n", block_no);
        blk = (Block*)kalloc(sizeof(Block));
        init_block(blk);
        blk->block_no = block_no;
        device_read(blk);
        blk->valid = true;

        increment_rc(&num_cached_blocks);
        insert_into_list(&lock, &head, &blk->node);
    }

    if(!acquire_sleeplock(&blk->lock)){
        return NULL;
    }
    blk->acquired = true;

    return blk;
}

// see `cache.h`.
static void cache_release(Block *block)
{
    block->acquired = false;
    release_sleeplock(&block->lock);
}

// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device)
{
    sblock = _sblock;
    device = _device;

    // TODO
    init_spinlock(&lock);
    init_rc(&num_cached_blocks);
    init_list_node(&head);
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx)
{
    // TODO
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block)
{
    // TODO
    if(!ctx){
        device_write(block);
    }
}

// see `cache.h`.
static void cache_end_op(OpContext *ctx)
{
    // TODO
}

// see `cache.h`.
static usize cache_alloc(OpContext *ctx)
{
    // TODO
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no)
{
    // TODO
}

BlockCache bcache = {
    .get_num_cached_blocks = get_num_cached_blocks,
    .acquire = cache_acquire,
    .release = cache_release,
    .begin_op = cache_begin_op,
    .sync = cache_sync,
    .end_op = cache_end_op,
    .alloc = cache_alloc,
    .free = cache_free,
};