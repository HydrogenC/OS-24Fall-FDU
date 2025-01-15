#include "file.h"
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/sem.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <common/list.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <common/string.h>

// the global file table.
static struct ftable ftable;

void init_ftable()
{
    // TODO: initialize your ftable.
    init_spinlock(&ftable.lock);

    // Mark all slots as empty
    for (usize i = 0; i < NFILE; i++) {
        ftable.files[i].ref = 0;
        ftable.files[i].type = FD_NONE;
    }
}

void init_oftable(struct oftable *oftable)
{
    // TODO: initialize your oftable for a new process.
    init_spinlock(&oftable->lock);
    memset(&oftable->files, 0, sizeof(oftable->files));
}

/* Allocate a file structure. */
struct file *file_alloc()
{
    /* (Final) TODO BEGIN */

    acquire_spinlock(&ftable.lock);

    // Find slot and return
    for (usize i = 0; i < NFILE; i++) {
        if (ftable.files[i].ref == 0) {
            ftable.files[i].ref++;
            release_spinlock(&ftable.lock);
            return &ftable.files[i];
        }
    }
    release_spinlock(&ftable.lock);

    /* (Final) TODO END */
    printk("PANIC: No file slot left! \n");
    return NULL;
}

/* Increment ref count for file f. */
struct file *file_dup(struct file *f)
{
    /* (Final) TODO BEGIN */

    acquire_spinlock(&ftable.lock);
    ASSERT(f->ref > 0);
    f->ref++;
    release_spinlock(&ftable.lock);

    /* (Final) TODO END */
    return f;
}

/* Close file f. (Decrement ref count, close when reaches 0.) */
void file_close(struct file *f)
{
    /* (Final) TODO BEGIN */

    acquire_spinlock(&ftable.lock);
    ASSERT(f->ref > 0);
    f->ref--;

    if (f->ref > 0) {
        release_spinlock(&ftable.lock);
        return;
    }

    // Recycle file
    ASSERT(f->ref == 0);
    auto file_type = f->type;
    f->type = FD_NONE;

    // Obtain the handle, then release the lock, to avoid the handle from being overwritten
    switch (file_type) {
    case FD_PIPE: {
        struct pipe *pipe = f->pipe;
        release_spinlock(&ftable.lock);
        // TODO: Close the pipe
    } break;
    case FD_INODE: {
        Inode *inode = f->ip;
        release_spinlock(&ftable.lock);

        // Deconstruct inode
        OpContext ctx;
        bcache.begin_op(&ctx);
        // Have to guarantee that the inode is unlocked
        inodes.put(&ctx, inode);
        bcache.end_op(&ctx);
    } break;
    default:
        release_spinlock(&ftable.lock);
        break;
    }

    /* (Final) TODO END */
}

/* Get metadata about file f. */
int file_stat(struct file *f, struct stat *st)
{
    /* (Final) TODO BEGIN */
    if (f->type == FD_INODE) {
        ASSERT(f->ip != NULL);
        inodes.lock(f->ip);
        stati(f->ip, st);
        inodes.unlock(f->ip);
        return 0;
    }

    /* (Final) TODO END */
    printk("WARNING: Getting stat of non-inode file\n");
    return -1;
}

/* Read from file f. */
isize file_read(struct file *f, char *addr, isize n)
{
    /* (Final) TODO BEGIN */
    if (!f->readable) {
        printk("(warn) Reading unreadable file\n");
        return -1;
    }

    switch (f->type) {
    case FD_INODE: {
        ASSERT(f->ip != NULL);
        inodes.lock(f->ip);
        usize bytes_read = inodes.read(f->ip, (u8 *)addr, f->off, n);
        f->off += bytes_read;
        inodes.unlock(f->ip);
        return bytes_read;
    } break;
    case FD_PIPE: {
        // TODO: pipe read
    } break;
    default:
        break;
    }

    /* (Final) TODO END */
    return 0;
}

/* Write to file f. */
isize file_write(struct file *f, char *addr, isize n)
{
    /* (Final) TODO BEGIN */
    if (!f->writable) {
        printk("(warn) Writing unwritable file\n");
        return -1;
    }

    switch (f->type) {
    case FD_INODE: {
        ASSERT(f->ip != NULL);
        
        OpContext ctx;
        bcache.begin_op(&ctx);
        inodes.lock(f->ip);
        usize bytes_written = inodes.write(&ctx, f->ip, (u8 *)addr, f->off, n);
        inodes.unlock(f->ip);
        bcache.end_op(&ctx);

        f->off += bytes_written;
        return bytes_written;
    } break;
    case FD_PIPE: {
        // TODO: pipe write
    } break;
    default:
        break;
    }

    /* (Final) TODO END */
    return 0;
}