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
    SpinLock lock;
    Semaphore sem;
    bool committing;
    int num_ops;
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
static void cache_evict()
{
    ListNode *node = head.prev;

    // Inverse traverse, find the cache that is used least recently (LRU)
    while (node != &head) {
        Block *current_blk = container_of(node, Block, node);
        node = node->prev;

        // Skip acquired and pinned blocks
        if (!current_blk->acquired && !current_blk->pinned) {
            // printk("Evicting block No. %llu\n", current_blk->block_no);
            _detach_from_list(&current_blk->node);
            decrement_rc(&num_cached_blocks);
            kfree(current_blk);

            // Keep evicting until below threshold
            if (num_cached_blocks.count < EVICTION_THRESHOLD) {
                break;
            }
        }
    }
}

Block *try_find_block(usize block_no)
{
    ListNode *node = head.next;

    while (node != &head) {
        Block *current_blk = container_of(node, Block, node);
        if (current_blk->block_no == block_no) {
            // printk("Found block No. %llu\n", current_blk->block_no);
            return current_blk;
        }

        node = node->next;
    }

    return NULL;
}

// see `cache.h`.
static Block *cache_acquire(usize block_no)
{
    // TODO
    acquire_spinlock(&lock);
    Block *blk = try_find_block(block_no);

    // Cache block not found, read from disk
    if (!blk) {
        if (get_num_cached_blocks() >= EVICTION_THRESHOLD) {
            cache_evict();
        }

        // printk("Initing block No. %llu\n", block_no);
        blk = (Block *)kalloc(sizeof(Block));
        init_block(blk);
        blk->block_no = block_no;
        device_read(blk);
        blk->valid = true;
        blk->acquired = true;

        increment_rc(&num_cached_blocks);
        _insert_into_list(&head, &blk->node);
    }
    blk->acquired = true;
    release_spinlock(&lock);

    if (!acquire_sleeplock(&blk->lock)) {
        return NULL;
    }

    return blk;
}

// see `cache.h`.
static void cache_release(Block *block)
{
    release_sleeplock(&block->lock);

    acquire_spinlock(&lock);
    block->acquired = false;
    // Move node to front of the list, so that it is recycled in time order (LRU)
    _detach_from_list(&block->node);
    _insert_into_list(&head, &block->node);
    release_spinlock(&lock);
}

void commit_log()
{
    // printk("Commiting log, number of blocks is %d\n", header.num_blocks);

    for (u64 i = 0; i < header.num_blocks; i++) {
        // Read data from log section
        Block log_blk;
        log_blk.block_no = sblock->log_start + i + 1;
        device_read(&log_blk);

        // Write to the actual place to store it
        log_blk.block_no = header.block_no[i];
        device_write(&log_blk);
    }

    // Modify metadata and write log into disk
    header.num_blocks = 0;
    write_header();
}

// see `cache.h`.
void init_bcache(const SuperBlock *_sblock, const BlockDevice *_device)
{
    sblock = _sblock;
    device = _device;

    // TODO
    init_spinlock(&lock);
    init_spinlock(&log.lock);
    init_sem(&log.sem, 1);
    init_rc(&num_cached_blocks);
    init_list_node(&head);

    log.committing = false;
    log.num_ops = 0;

    read_header();
    commit_log();
}

// see `cache.h`.
static void cache_begin_op(OpContext *ctx)
{
    // TODO
    acquire_spinlock(&log.lock);
    while (log.committing ||
           (log.num_ops + 1) * OP_MAX_NUM_BLOCKS > LOG_MAX_SIZE) {
        release_spinlock(&log.lock);
        // Process already killed, no op required any more
        if (!wait_sem(&log.sem)) {
            return;
        }
        acquire_spinlock(&log.lock);
    }

    log.num_ops++;
    ctx->rm = OP_MAX_NUM_BLOCKS;
    // ctx->ts = get_timestamp();
    release_spinlock(&log.lock);
}

// see `cache.h`.
static void cache_sync(OpContext *ctx, Block *block)
{
    // TODO
    if (!ctx) {
        device_write(block);
    } else {
        acquire_spinlock(&log.lock);

        // Skip blocks that are already marked as dirty
        if (block->pinned) {
            release_spinlock(&log.lock);
            return;
        }

        if (ctx->rm <= 0) {
            PANIC();
        }
        ctx->rm--;

        // Pin block and write header
        block->pinned = true;
        header.block_no[header.num_blocks++] = block->block_no;
        release_spinlock(&log.lock);
    }
}

// see `cache.h`.
static void cache_end_op(OpContext *ctx)
{
    // TODO
    acquire_spinlock(&log.lock);
    if (log.committing) {
        // Calling end op while still commiting shall not happen
        PANIC();
    }

    log.num_ops--;
    if (log.num_ops > 0) {
        // There are still ops that haven't finished, do not commit
        // But we can call wake up `begin_op`s
        post_all_sem(&log.sem);
        // printk("Ended op, remaining %d\n", log.num_ops);
        release_spinlock(&log.lock);
        return;
    }

    log.committing = true;
    // printk("Ended op, Writing to disk\n", log.num_ops);
    release_spinlock(&log.lock);

    // Write data into log
    for (u64 i = 0; i < header.num_blocks; i++) {
        // Read data from cache
        acquire_spinlock(&lock);
        Block *blk = try_find_block(header.block_no[i]);
        if (!blk) {
            PANIC();
        }

        // Write to log area
        device->write(sblock->log_start + i + 1, blk->data);
        blk->pinned = false;
        release_spinlock(&lock);
    }

    write_header();
    commit_log();

    acquire_spinlock(&log.lock);
    log.committing = false;
    post_all_sem(&log.sem);
    release_spinlock(&log.lock);
}

void __debug_print_bitmap_block(Block *bitmap_block)
{
    for (usize j = 0; j < BLOCK_SIZE; j += 8) {
        u64 *num = &(bitmap_block->data[j]);
        printk("%llu ", *num);
    }
    printk("\n");
}

// see `cache.h`.
static usize cache_alloc(OpContext *ctx)
{
    // TODO
    if (!ctx) {
        PANIC();
    }

    Block *bitmap_block;
    for (usize i = 0; i < sblock->num_blocks; i += BIT_PER_BLOCK) {
        const usize bitmap_block_no = sblock->bitmap_start + i / BIT_PER_BLOCK;
        bitmap_block = cache_acquire(bitmap_block_no);

        for (usize j = 0; j < BIT_PER_BLOCK && i + j < sblock->num_blocks;
             j++) {
            u8 probe = 1u << (j % 8u);
            // If block is free
            if ((bitmap_block->data[j / 8] & probe) == 0) {
                bitmap_block->data[j / 8] |= probe;
                cache_sync(ctx, bitmap_block);
                cache_release(bitmap_block);
                usize block_no = i + j;

                // Use buffer as a zero-buffer to clear the block
                u8 zero_buffer[BLOCK_SIZE];
                memset(zero_buffer, 0, BLOCK_SIZE);
                device->write(block_no, zero_buffer);
                // printk("Allocating block %d\n", block_no);
                return block_no;
            }
        }

        cache_release(bitmap_block);
    }

    printk("PANIC: No free block remaining. \n");
    PANIC();
}

// see `cache.h`.
static void cache_free(OpContext *ctx, usize block_no)
{
    const bitmap_block_no = sblock->bitmap_start + block_no / BIT_PER_BLOCK;
    // printk("Freeing block %d\n", block_no);
    Block *bitmap_block = cache_acquire(bitmap_block_no);

    usize in_block_index = block_no % BIT_PER_BLOCK;
    u8 probe = 1u << (in_block_index % 8u);
    bitmap_block->data[in_block_index / 8] &= ~probe;
    cache_sync(ctx, bitmap_block);
    cache_release(bitmap_block);
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