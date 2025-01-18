#include <common/string.h>
#include <fs/inode.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <fs/cache.h>
#include <kernel/sched.h>
#include <kernel/console.h>

/**
    @brief the private reference to the super block.

    @note we need these two variables because we allow the caller to
            specify the block cache and super block to use.
            Correspondingly, you should NEVER use global instance of
            them.

    @see init_inodes
 */
static const SuperBlock *sblock;

/**
    @brief the reference to the underlying block cache.
 */
static const BlockCache *cache;

/**
    @brief global lock for inode layer.

    Use it to protect anything you need.

    e.g. the list of allocated blocks, ref counts, etc.
 */
static SpinLock lock;

/**
    @brief the list of all allocated in-memory inodes.

    We use a linked list to manage all allocated inodes.

    You can implement your own data structure if you want better performance.

    @see Inode
 */
static ListNode head;

// return which block `inode_no` lives on.
static INLINE usize to_block_no(usize inode_no)
{
    return sblock->inode_start + (inode_no / (INODE_PER_BLOCK));
}

// return the pointer to on-disk inode.
static INLINE InodeEntry *get_entry(Block *block, usize inode_no)
{
    return ((InodeEntry *)block->data) + (inode_no % INODE_PER_BLOCK);
}

// return address array in indirect block.
static INLINE u32 *get_addrs(Block *block)
{
    return ((IndirectBlock *)block->data)->addrs;
}

// initialize inode tree.
void init_inodes(const SuperBlock *_sblock, const BlockCache *_cache)
{
    init_spinlock(&lock);
    init_list_node(&head);
    sblock = _sblock;
    cache = _cache;

    if (ROOT_INODE_NO < sblock->num_inodes)
        inodes.root = inodes.get(ROOT_INODE_NO);
    else
        printk("(warn) init_inodes: no root inode.\n");
}

// initialize in-memory inode.
static void init_inode(Inode *inode)
{
    init_sleeplock(&inode->lock);
    init_rc(&inode->rc);
    init_list_node(&inode->node);
    inode->inode_no = 0;
    inode->valid = false;
}

// see `inode.h`.
static usize inode_alloc(OpContext *ctx, InodeType type)
{
    ASSERT(type != INODE_INVALID);

    // TODO
    Block *inode_block = cache->acquire(to_block_no(ROOT_INODE_NO));
    for (usize i = 1; i < sblock->num_inodes; i++) {
        // If it's the first inode within block, load the block
        if (i % INODE_PER_BLOCK == 0) {
            inode_block = cache->acquire(to_block_no(i));
        }

        InodeEntry *entry = get_entry(inode_block, i);
        if (entry->type == 0) {
            // Initialize with zero
            memset(entry, 0, sizeof(InodeEntry));
            // Set type to mark inode as in use
            entry->type = type;
            cache->sync(ctx, inode_block);
            cache->release(inode_block);
            return i;
        }

        // If it's the last inode within block or in inode area, release the cache
        if (i % INODE_PER_BLOCK == INODE_PER_BLOCK - 1 ||
            i == sblock->num_inodes - 1) {
            cache->release(inode_block);
        }
    }

    // No inode found
    printk("(warn) inode_alloc: no free inode.\n");
    return 0;
}

// see `inode.h`.
static void inode_lock(Inode *inode)
{
    ASSERT(inode->rc.count > 0);
    // TODO
    ASSERT(acquire_sleeplock(&inode->lock));

    // Load from disk if not present
    if (!inode->valid) {
        Block *inode_block = cache->acquire(to_block_no(inode->inode_no));
        InodeEntry *inodes = (InodeEntry *)inode_block->data;

        // Load data into
        memcpy(&inode->entry, &inodes[inode->inode_no % INODE_PER_BLOCK],
               sizeof(InodeEntry));
        cache->release(inode_block);
        inode->valid = true;
    }
}

// see `inode.h`.
static void inode_unlock(Inode *inode)
{
    ASSERT(inode->rc.count > 0);
    // TODO
    release_sleeplock(&inode->lock);
}

// see `inode.h`.
static void inode_sync(OpContext *ctx, Inode *inode, bool do_write)
{
    // TODO
    Block *inode_block = cache->acquire(to_block_no(inode->inode_no));
    InodeEntry *inodes = (InodeEntry *)inode_block->data;
    usize inode_index = inode->inode_no % INODE_PER_BLOCK;

    if (do_write) {
        // Cannot write invalid data
        ASSERT(inode->valid);

        // Write data to disk
        memcpy(&inodes[inode_index], &inode->entry, sizeof(InodeEntry));
        cache->sync(ctx, inode_block);
        cache->release(inode_block);
    } else if (!inode->valid) {
        // Read data from disk if not present
        memcpy(&inode->entry, &inodes[inode_index], sizeof(InodeEntry));
        cache->release(inode_block);
        inode->valid = true;
    } else {
        // Do nothing if data is present and not `do_write`
        cache->release(inode_block);
    }
}

Inode *try_find_inode(usize inode_no)
{
    ListNode *node = head.next;

    while (node != &head) {
        Inode *current_inode = container_of(node, Inode, node);
        if (current_inode->inode_no == inode_no) {
            // printk("Found block No. %llu\n", current_blk->block_no);
            return current_inode;
        }

        node = node->next;
    }

    return NULL;
}

// see `inode.h`.
static Inode *inode_get(usize inode_no)
{
    ASSERT(inode_no > 0);
    ASSERT(inode_no < sblock->num_inodes);
    acquire_spinlock(&lock);
    // TODO
    Inode *inode = try_find_inode(inode_no);

    // Alloc new node and init
    if (!inode) {
        inode = (Inode *)kalloc(sizeof(Inode));
        init_inode(inode);

        inode->inode_no = inode_no;
        _insert_into_list(&head, &inode->node);
    }

    increment_rc(&inode->rc);
    release_spinlock(&lock);
    return inode;
}
// see `inode.h`.
static void inode_clear(OpContext *ctx, Inode *inode)
{
    // TODO
    u32 *direct_addrs = inode->entry.addrs;
    for (u64 i = 0; i < INODE_NUM_DIRECT; i++) {
        if (direct_addrs[i]) {
            // printk("Freeing direct data block %d\n", direct_addrs[i]);
            cache->free(ctx, direct_addrs[i]);
            direct_addrs[i] = 0;
        }
    }

    // If has indirect block, then clear it
    if (inode->entry.indirect) {
        Block *indirect_blk = cache->acquire(inode->entry.indirect);
        u32 *indirect_addrs = get_addrs(indirect_blk);
        for (u64 i = 0; i < INODE_NUM_INDIRECT; i++) {
            if (indirect_addrs[i]) {
                // printk("Freeing indirect data block %d\n", indirect_addrs[i]);
                cache->free(ctx, indirect_addrs[i]);
                indirect_addrs[i] = 0;
            }
        }

        cache->release(indirect_blk);
        cache->free(ctx, inode->entry.indirect);
        inode->entry.indirect = 0;
    }

    inode->entry.num_bytes = 0;
    inode_sync(ctx, inode, true);
}

// see `inode.h`.
static Inode *inode_share(Inode *inode)
{
    // TODO
    ASSERT(inode != NULL);
    increment_rc(&inode->rc);
    return inode;
}

// see `inode.h`.
static void inode_put(OpContext *ctx, Inode *inode)
{
    // TODO
    if (!inode->valid) {
        inode_sync(ctx, inode, false);
    }

    acquire_spinlock(&lock);
    decrement_rc(&inode->rc);
    if (inode->rc.count == 0) {
        // Clear the inode if no num_links
        if (inode->entry.num_links == 0) {
            inode_clear(ctx, inode);
            // Set inode entry as unused
            inode->entry.type = INODE_INVALID;
            inode_sync(ctx, inode, true);
            inode->valid = false;
        }

        // If no remaining references, then free the inode itself
        _detach_from_list(&inode->node);
        kfree(inode);
    }

    release_spinlock(&lock);
}

/**
    @brief get which block is the offset of the inode in.

    e.g. `inode_map(ctx, my_inode, 1234, &modified)` will return the block_no
    of the block that contains the 1234th byte of the file
    represented by `my_inode`.

    If a block has not been allocated for that byte, `inode_map` will
    allocate a new block and update `my_inode`, at which time, `modified`
    will be set to true.

    HOWEVER, if `ctx == NULL`, `inode_map` will NOT try to allocate any new block,
    and when it finds that the block has not been allocated, it will return 0.
    
    @param[out] modified true if some new block is allocated and `inode`
    has been changed.

    @return usize the block number of that block, or 0 if `ctx == NULL` and
    the required block has not been allocated.

    @note the caller must hold the lock of `inode`.
 */
static usize inode_map(OpContext *ctx, Inode *inode, usize offset,
                       bool *modified)
{
    // TODO
    // Within direct blocks
    *modified = false;
    if (offset < BLOCK_SIZE * INODE_NUM_DIRECT) {
        u32 block_index = offset / BLOCK_SIZE;
        if (inode->entry.addrs[block_index] == 0) {
            // No `ctx`, cannot alloc
            if (!ctx) {
                printk("(warn) inode_map: no ctx, cannot create direct data block.\n");
                return 0;
            }

            inode->entry.addrs[block_index] = cache->alloc(ctx);
            *modified = true;
        }
        return inode->entry.addrs[block_index];
    }

    // Within indirect block
    offset -= BLOCK_SIZE * INODE_NUM_DIRECT;
    if (offset < BLOCK_SIZE * INODE_NUM_INDIRECT) {
        // No indirect block, try alloc
        if (inode->entry.indirect == 0) {
            if (!ctx) {
                printk("(warn) inode_map: no ctx, cannot create indirect table block.\n");
                return 0;
            }

            inode->entry.indirect = cache->alloc(ctx);
            *modified = true;
        }

        Block *indirect_blk = cache->acquire(inode->entry.indirect);
        u32 block_index = offset / BLOCK_SIZE;

        u32 *indirect_addrs = get_addrs(indirect_blk);
        if (indirect_addrs[block_index] == 0) {
            if (!ctx) {
                printk("(warn) inode_map: no ctx, cannot create indirect data block.\n");
                return 0;
            }

            indirect_addrs[block_index] = cache->alloc(ctx);
            cache->sync(ctx, indirect_blk);
            *modified = true;
        }

        cache->release(indirect_blk);
        return indirect_addrs[block_index];
    }

    // File too large
    printk("(warn) inode_map: file too large.\n");
    return 0;
}

// see `inode.h`.
static usize inode_read(Inode *inode, u8 *dest, usize offset, usize count)
{
    ASSERT(inode != NULL);
    InodeEntry *entry = &inode->entry;

    if (entry->type == INODE_DEVICE) {
        return console_read(inode, (char *)dest, count);
    }

    if (count + offset > entry->num_bytes)
        count = entry->num_bytes - offset;
    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);

    // Clamp to end of file
    if (end > entry->num_bytes) {
        end = entry->num_bytes;
    }

    // TODO
    usize pos = offset;
    while (pos < end) {
        bool modified;
        u32 data_blk_no = inode_map(NULL, inode, pos, &modified);
        // Since `ctx` is NULL, modified should have been `false`
        ASSERT(!modified);

        // Cannot read current data block
        if (data_blk_no == 0) {
            // Terminate read and return bytes already read
            printk("(warn) cannot read data block, aborting. \n");
            return pos - offset;
        }

        Block *data_blk = cache->acquire(data_blk_no);
        u32 pos_in_block = pos % BLOCK_SIZE;
        u32 read_count = MIN(BLOCK_SIZE - pos_in_block, end - pos);
        memcpy(dest, &data_blk->data[pos_in_block], read_count);

        // printk("Read %d bytes from block %d at pos %d\n", read_count,
        //        data_blk_no, pos);

        cache->release(data_blk);
        pos += read_count;
        dest += read_count;
    }

    return pos - offset;
}

// see `inode.h`.
static usize inode_write(OpContext *ctx, Inode *inode, u8 *src, usize offset,
                         usize count)
{
    ASSERT(inode != NULL);
    ASSERT(ctx != NULL);
    ASSERT(count <= OP_MAX_NUM_BLOCKS * BLOCK_SIZE);
    InodeEntry *entry = &inode->entry;

    if (entry->type == INODE_DEVICE) {
        return console_write(inode, (char *)src, count);
    }

    usize end = offset + count;
    ASSERT(offset <= entry->num_bytes);
    ASSERT(end <= INODE_MAX_BYTES);
    ASSERT(offset <= end);

    // TODO
    bool inode_needs_sync = false;
    usize pos = offset;
    while (pos < end) {
        bool modified;
        u32 data_blk_no = inode_map(ctx, inode, pos, &modified);
        inode_needs_sync = inode_needs_sync || modified;

        // Cannot read current data block
        if (data_blk_no == 0) {
            // Terminate read and return bytes already read
            printk("(warn) cannot write data block, aborting. \n");
            return pos - offset;
        }

        Block *data_blk = cache->acquire(data_blk_no);
        u32 pos_in_block = pos % BLOCK_SIZE;
        u32 write_count = MIN(BLOCK_SIZE - pos_in_block, end - pos);
        memcpy(&data_blk->data[pos_in_block], src, write_count);

        // printk("Written %d bytes to block %d at pos %d\n", write_count,
        //        data_blk_no, pos);

        cache->sync(ctx, data_blk);
        cache->release(data_blk);
        pos += write_count;
        src += write_count;
    }

    // If appended, modify size
    if (pos > inode->entry.num_bytes) {
        inode->entry.num_bytes = pos;
        inode_needs_sync = true;
    }

    if (inode_needs_sync) {
        inode_sync(ctx, inode, true);
    }

    return pos - offset;
}

// see `inode.h`.
static usize inode_lookup(Inode *inode, const char *name, usize *index)
{
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    u32 len_name = strlen(name);
    DirEntry dir_entry;
    for (u32 i = 0; i < inode->entry.num_bytes; i += sizeof(DirEntry)) {
        usize read_size =
                inode_read(inode, (u8 *)&dir_entry, i, sizeof(DirEntry));
        ASSERT(read_size == sizeof(DirEntry));

        // Empty item
        if (dir_entry.inode_no == 0) {
            continue;
        }
        // printk("found file: %s\n", dir_entry.name);

        u32 len_entry_name = strlen(dir_entry.name);
        // Two string must not be equal
        if (len_entry_name != len_name) {
            continue;
        }

        // File name is correct
        if (strncmp(name, dir_entry.name, len_entry_name) == 0) {
            if (index) {
                *index = i / sizeof(DirEntry);
            }
            return dir_entry.inode_no;
        }
    }

    // Not found
    return 0;
}

// see `inode.h`.
static isize inode_insert(OpContext *ctx, Inode *inode, const char *name,
                          usize inode_no)
{
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    usize dir_index;
    u32 block_no = inode_lookup(inode, name, &dir_index);

    // Already exists
    if (block_no != 0) {
        return -1;
    }

    DirEntry dir_entry;
    isize entry_offset = 0;
    for (; entry_offset < inode->entry.num_bytes;
         entry_offset += sizeof(DirEntry)) {
        usize read_size = inode_read(inode, (u8 *)&dir_entry, entry_offset,
                                     sizeof(DirEntry));
        ASSERT(read_size == sizeof(DirEntry));

        // Empty item
        if (dir_entry.inode_no == 0) {
            break;
        }
    }

    // Note: if there's no empty space, then `entry_offset` will be at the end position of inode,
    // then the size of the directory inode would grow when calling `inode_write`
    dir_entry.inode_no = inode_no;
    // Ensure length does not exceed buffer
    ASSERT(strlen(name) <= FILE_NAME_MAX_LENGTH - 1);
    memcpy(dir_entry.name, name, strlen(name) + 1);

    // Test if write succeeds
    if (inode_write(ctx, inode, (u8 *)&dir_entry, entry_offset,
                    sizeof(DirEntry)) != sizeof(DirEntry)) {
        printk("(warn) inode insertion failed due to write fault\n");
        return -1;
    }
    return entry_offset / sizeof(DirEntry);
}

// see `inode.h`.
static void inode_remove(OpContext *ctx, Inode *inode, usize index)
{
    InodeEntry *entry = &inode->entry;
    ASSERT(entry->type == INODE_DIRECTORY);

    // TODO
    DirEntry dir_entry;
    usize read_size = inode_read(inode, (u8 *)&dir_entry,
                                 index * sizeof(DirEntry), sizeof(DirEntry));
    ASSERT(read_size == sizeof(DirEntry));

    if (dir_entry.inode_no != 0) {
        // TODO: Remove the inode of `inode_no` if applicable, but this is not necessary in this lab
        dir_entry.inode_no = 0;
        inode_write(ctx, inode, (u8 *)&dir_entry, index * sizeof(DirEntry),
                    sizeof(DirEntry));
    }
}

InodeTree inodes = {
    .alloc = inode_alloc,
    .lock = inode_lock,
    .unlock = inode_unlock,
    .sync = inode_sync,
    .get = inode_get,
    .clear = inode_clear,
    .share = inode_share,
    .put = inode_put,
    .read = inode_read,
    .write = inode_write,
    .lookup = inode_lookup,
    .insert = inode_insert,
    .remove = inode_remove,
};

/**
    @brief read the next path element from `path` into `name`.
    
    @param[out] name next path element.

    @return const char* a pointer offseted in `path`, without leading `/`. If no
    name to remove, return NULL.

    @example 
    skipelem("a/bb/c", name) = "bb/c", setting name = "a",
    skipelem("///a//bb", name) = "bb", setting name = "a",
    skipelem("a", name) = "", setting name = "a",
    skipelem("", name) = skipelem("////", name) = NULL, not setting name.
 */
static const char *skipelem(const char *path, char *name)
{
    const char *s;
    int len;

    while (*path == '/')
        path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    len = path - s;
    if (len >= FILE_NAME_MAX_LENGTH)
        memmove(name, s, FILE_NAME_MAX_LENGTH);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/')
        path++;
    return path;
}

/**
    @brief look up and return the inode for `path`.

    If `nameiparent`, return the inode for the parent and copy the final
    path element into `name`.
    
    @param path a relative or absolute path. If `path` is relative, it is
    relative to the current working directory of the process.

    @param[out] name the final path element if `nameiparent` is true.

    @return Inode* the inode for `path` (or its parent if `nameiparent` is true), 
    or NULL if such inode does not exist.

    @example
    namex("/a/b", false, name) = inode of b,
    namex("/a/b", true, name) = inode of a, setting name = "b",
    namex("/", true, name) = NULL (because "/" has no parent!)
 */
static Inode *namex(const char *path, bool nameiparent, char *name,
                    OpContext *ctx)
{
    /* (Final) TODO BEGIN */

    Inode *current = NULL;

    // Absolute path
    if (path[0] == '/') {
        current = inode_share(inodes.root);
    } else {
        // Increment rc when getting the path
        current = inode_share(thisproc()->cwd);
    }

    path = skipelem(path, name);
    while (path != 0) {
        // Load data into memory
        inode_lock(current);

        if (current->entry.type != INODE_DIRECTORY) {
            inode_unlock(current);
            inode_put(ctx, current);
            printk("(warn) calling `namex` on non-directory inode! \n");
            return NULL;
        }

        // `current` is already the dir being looked for
        if (path[0] == '\0' && nameiparent) {
            inode_unlock(current);
            return current;
        }

        // Find next level
        usize next_no = inode_lookup(current, name, NULL);
        if (next_no == 0) {
            inode_unlock(current);
            inode_put(ctx, current);
            printk("(warn) next dir `%s` not found! \n", name);
            return NULL;
        }

        Inode *next = inode_get(next_no);
        // This shall not fail since `inode_no` has already proved to be valid
        ASSERT(next != NULL);

        // Deconstruct current dir level
        inode_unlock(current);
        inode_put(ctx, current);

        // Goto next level
        current = next;
        path = skipelem(path, name);
    }

    if (nameiparent) {
        inode_put(ctx, current);
        return 0;
    }

    /* (Final) TODO END */
    return current;
}

Inode *namei(const char *path, OpContext *ctx)
{
    char name[FILE_NAME_MAX_LENGTH];
    return namex(path, false, name, ctx);
}

Inode *nameiparent(const char *path, char *name, OpContext *ctx)
{
    return namex(path, true, name, ctx);
}

/**
    @brief get the stat information of `ip` into `st`.
    
    @note the caller must hold the lock of `ip`.
 */
void stati(Inode *ip, struct stat *st)
{
    st->st_dev = 1;
    st->st_ino = ip->inode_no;
    st->st_nlink = ip->entry.num_links;
    st->st_size = ip->entry.num_bytes;
    switch (ip->entry.type) {
    case INODE_REGULAR:
        st->st_mode = S_IFREG;
        break;
    case INODE_DIRECTORY:
        st->st_mode = S_IFDIR;
        break;
    case INODE_DEVICE:
        st->st_mode = 0;
        break;
    default:
        PANIC();
    }
}