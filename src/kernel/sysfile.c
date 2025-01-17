//
// File-system system calls implementation.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <stddef.h>

#include "syscall.h"
#include <aarch64/mmu.h>
#include <common/defines.h>
#include <common/spinlock.h>
#include <common/string.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/pipe.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

struct iovec {
    void *iov_base; /* Starting address. */
    usize iov_len; /* Number of bytes to transfer. */
};

/** 
 * Get the file object by fd. Return null if the fd is invalid.
 */
static struct file *fd2file(int fd)
{
    /* (Final) TODO BEGIN */

    Proc *this = thisproc();

    // Avoid index out of bound
    if (fd >= NFILE || fd < 0) {
        return NULL;
    }

    File *file = this->oftable.files[fd];
    if (file == NULL || file->type == FD_NONE) {
        return NULL;
    }

    return file;

    /* (Final) TODO END */
}

/*
 * Allocate a file descriptor for the given file.
 * Takes over file reference from caller on success.
 */
int fdalloc(struct file *f)
{
    /* (Final) TODO BEGIN */

    Proc *this = thisproc();
    acquire_spinlock(&this->oftable.lock);
    for (usize i = 0; i < NFILE; i++) {
        if (this->oftable.files[i] == NULL) {
            this->oftable.files[i] = f;
            release_spinlock(&this->oftable.lock);
            return i;
        }
    }
    release_spinlock(&this->oftable.lock);

    /* (Final) TODO END */
    return -1;
}

define_syscall(ioctl, int fd, u64 request)
{
    // 0x5413 is TIOCGWINSZ (I/O Control to Get the WINdow SIZe, a magic request
    // to get the stdin terminal size) in our implementation. Just ignore it.
    ASSERT(request == 0x5413);
    (void)fd;
    return 0;
}

define_syscall(mmap, void *addr, int length, int prot, int flags, int fd,
               int offset)
{
    /* (Final) TODO BEGIN */

    /* (Final) TODO END */
}

define_syscall(munmap, void *addr, size_t length)
{
    /* (Final) TODO BEGIN */

    /* (Final) TODO END */
}

define_syscall(dup, int fd)
{
    struct file *f = fd2file(fd);
    if (!f)
        return -1;
    fd = fdalloc(f);
    if (fd < 0)
        return -1;
    file_dup(f);
    return fd;
}

define_syscall(read, int fd, char *buffer, int size)
{
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_writeable(buffer, size))
        return -1;
    return file_read(f, buffer, size);
}

define_syscall(write, int fd, char *buffer, int size)
{
    struct file *f = fd2file(fd);
    if (!f || size <= 0 || !user_readable(buffer, size))
        return -1;
    return file_write(f, buffer, size);
}

define_syscall(writev, int fd, struct iovec *iov, int iovcnt)
{
    struct file *f = fd2file(fd);
    struct iovec *p;
    if (!f || iovcnt <= 0 || !user_readable(iov, sizeof(struct iovec) * iovcnt))
        return -1;
    usize tot = 0;
    for (p = iov; p < iov + iovcnt; p++) {
        if (!user_readable(p->iov_base, p->iov_len))
            return -1;
        tot += file_write(f, p->iov_base, p->iov_len);
    }
    return tot;
}

define_syscall(close, int fd)
{
    /* (Final) TODO BEGIN */
    File *f = fd2file(fd);

    if (f == NULL) {
        return -1;
    }

    thisproc()->oftable.files[fd] = 0;
    file_close(f);

    /* (Final) TODO END */
    return 0;
}

define_syscall(fstat, int fd, struct stat *st)
{
    struct file *f = fd2file(fd);
    if (!f || !user_writeable(st, sizeof(*st)))
        return -1;
    return file_stat(f, st);
}

define_syscall(newfstatat, int dirfd, const char *path, struct stat *st,
               int flags)
{
    if (!user_strlen(path, 256) || !user_writeable(st, sizeof(*st)))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_fstatat: dirfd unimplemented\n");
        return -1;
    }
    if (flags != 0) {
        printk("sys_fstatat: flags unimplemented\n");
        return -1;
    }

    Inode *ip;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = namei(path, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.lock(ip);
    stati(ip, st);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);

    return 0;
}

static int isdirempty(Inode *dp)
{
    usize off;
    DirEntry de;

    for (off = 2 * sizeof(de); off < dp->entry.num_bytes; off += sizeof(de)) {
        if (inodes.read(dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
            PANIC();
        if (de.inode_no != 0)
            return 0;
    }
    return 1;
}

define_syscall(unlinkat, int fd, const char *path, int flag)
{
    ASSERT(fd == AT_FDCWD && flag == 0);
    Inode *ip, *dp;
    DirEntry de;
    char name[FILE_NAME_MAX_LENGTH];
    usize off;
    if (!user_strlen(path, 256))
        return -1;
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((dp = nameiparent(path, name, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(dp);

    // Cannot unlink "." or "..".
    if (strncmp(name, ".", FILE_NAME_MAX_LENGTH) == 0 ||
        strncmp(name, "..", FILE_NAME_MAX_LENGTH) == 0)
        goto bad;

    usize inumber = inodes.lookup(dp, name, &off);
    if (inumber == 0)
        goto bad;
    ip = inodes.get(inumber);
    inodes.lock(ip);

    if (ip->entry.num_links < 1)
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY && !isdirempty(ip)) {
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        goto bad;
    }

    memset(&de, 0, sizeof(de));
    if (inodes.write(&ctx, dp, (u8 *)&de, off, sizeof(de)) != sizeof(de))
        PANIC();
    if (ip->entry.type == INODE_DIRECTORY) {
        dp->entry.num_links--;
        inodes.sync(&ctx, dp, true);
    }
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    ip->entry.num_links--;
    inodes.sync(&ctx, ip, true);
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;

bad:
    inodes.unlock(dp);
    inodes.put(&ctx, dp);
    bcache.end_op(&ctx);
    return -1;
}

/**
    @brief create an inode at `path` with `type`.

    If the inode exists, just return it.

    If `type` is directory, you should also create "." and ".." entries and link
   them with the new inode.

    @note BE careful of handling error! You should clean up ALL the resources
   you allocated and free ALL acquired locks when error occurs. e.g. if you
   allocate a new inode "/my/dir", but failed to create ".", you should free the
   inode "/my/dir" before return.

    @see `nameiparent` will find the parent directory of `path`.

    @return Inode* the created inode, or NULL if failed.
 */
Inode *create(const char *path, short type, short major, short minor,
              OpContext *ctx)
{
    /* (Final) TODO BEGIN */

    char name[FILE_NAME_MAX_LENGTH];

    Inode *parent = nameiparent(path, name, ctx);
    // Parent dir not found
    if (!parent) {
        return NULL;
    }
    inodes.lock(parent);

    usize inode_index = inodes.lookup(parent, name, NULL);
    if (inode_index > 0) {
        inodes.unlock(parent);
        inodes.put(ctx, parent);
        Inode *target = inodes.get(inode_index);
        inodes.lock(target);

        // Check if type matches and if type is valid
        if ((type == INODE_REGULAR || type == INODE_DIRECTORY) &&
            type == target->entry.type) {
            return target;
        }

        inodes.unlock(target);
        inodes.put(ctx, target);
        // Type mismatch or type invalid (only creating files and dirs are allowed)
        return NULL;
    }

    inode_index = inodes.alloc(ctx, type);
    if (inode_index == 0) {
        printk("PANIC: failed to alloc inode\n");
        inodes.unlock(parent);
        inodes.put(ctx, parent);
        return NULL;
    }

    Inode *target = inodes.get(inode_index);
    inodes.lock(target);

    target->entry.major = major;
    target->entry.minor = minor;
    target->entry.num_links = 1;
    inodes.sync(ctx, target, true);

    // Create `.` and `..`
    if (type == INODE_DIRECTORY) {
        if (inodes.insert(ctx, target, ".", target->inode_no) < 0 ||
            inodes.insert(ctx, target, "..", parent->inode_no) < 0) {
            printk("(warn) failed to alloc . or ..\n");

            // Deconstruct parent
            inodes.unlock(parent);
            inodes.put(ctx, parent);

            // Deconstruct self
            inodes.clear(ctx, target);
            inodes.unlock(target);
            inodes.put(ctx, target);
            return NULL;
        }

        // Increment ref of parent due to `..`
        parent->entry.num_links++;
        inodes.sync(ctx, parent, true);
    }

    if (inodes.insert(ctx, parent, name, target->inode_no) < 0) {
        printk("(warn) failed to append new entry to parent\n");

        // Deconstruct parent
        inodes.unlock(parent);
        inodes.put(ctx, parent);

        // Deconstruct self
        inodes.clear(ctx, target);
        inodes.unlock(target);
        inodes.put(ctx, target);
        return NULL;
    }

    // Deconstruct parent
    inodes.unlock(parent);
    inodes.put(ctx, parent);
    return target;

    /* (Final) TODO END */
}

define_syscall(openat, int dirfd, const char *path, int omode)
{
    int fd;
    struct file *f;
    Inode *ip;

    if (!user_strlen(path, 256))
        return -1;

    if (dirfd != AT_FDCWD) {
        printk("sys_openat: dirfd unimplemented\n");
        return -1;
    }

    OpContext ctx;
    bcache.begin_op(&ctx);
    if (omode & O_CREAT) {
        // FIXME: Support acl mode.
        ip = create(path, INODE_REGULAR, 0, 0, &ctx);
        if (ip == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
    } else {
        if ((ip = namei(path, &ctx)) == 0) {
            bcache.end_op(&ctx);
            return -1;
        }
        inodes.lock(ip);
    }

    if ((f = file_alloc()) == 0 || (fd = fdalloc(f)) < 0) {
        if (f)
            file_close(f);
        inodes.unlock(ip);
        inodes.put(&ctx, ip);
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    bcache.end_op(&ctx);

    f->type = FD_INODE;
    f->ip = ip;
    f->off = 0;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    return fd;
}

define_syscall(mkdirat, int dirfd, const char *path, int mode)
{
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mkdirat: dirfd unimplemented\n");
        return -1;
    }
    if (mode != 0) {
        printk("sys_mkdirat: mode unimplemented\n");
        return -1;
    }
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DIRECTORY, 0, 0, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(mknodat, int dirfd, const char *path, mode_t mode, dev_t dev)
{
    Inode *ip;
    if (!user_strlen(path, 256))
        return -1;
    if (dirfd != AT_FDCWD) {
        printk("sys_mknodat: dirfd unimplemented\n");
        return -1;
    }

    unsigned int ma = major(dev);
    unsigned int mi = minor(dev);
    printk("mknodat: path '%s', major:minor %u:%u\n", path, ma, mi);
    OpContext ctx;
    bcache.begin_op(&ctx);
    if ((ip = create(path, INODE_DEVICE, (short)ma, (short)mi, &ctx)) == 0) {
        bcache.end_op(&ctx);
        return -1;
    }
    inodes.unlock(ip);
    inodes.put(&ctx, ip);
    bcache.end_op(&ctx);
    return 0;
}

define_syscall(chdir, const char *path)
{
    /**
     * (Final) TODO BEGIN 
     * 
     * Change the cwd (current working dictionary) of current process to 'path'.
     * You may need to do some validations.
     */

    Proc *this = thisproc();

    OpContext ctx;
    bcache.begin_op(&ctx);

    Inode *inode = namei(path, &ctx);
    if (inode == NULL) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(inode);

    // Must be directory
    if (inode->entry.type != INODE_DIRECTORY) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.unlock(inode);
    inodes.put(&ctx, this->cwd);
    bcache.end_op(&ctx);

    this->cwd = inode;
    return 0;
    /* (Final) TODO END */
}

define_syscall(pipe2, int pipefd[2], int flags)
{
    /* (Final) TODO BEGIN */
    File *f0, *f1;
    if (pipe_alloc(&f0, &f1) < 0) {
        return -1;
    }

    pipefd[0] = pipefd[1] = -1;
    pipefd[0] = fdalloc(f0);
    if (pipefd[0] < 0) {
        goto failure;
    }
    
    pipefd[1] = fdalloc(f1);
    if (pipefd[1] < 0) {
        goto failure;
    }

    return 0;

failure:
    pipe_close(f0->pipe, 0);
    pipe_close(f0->pipe, 1);

    if (pipefd[0] >= 0) {
        sys_close(pipefd[0]);
    } else {
        file_close(f0);
    }

    if (pipefd[1] >= 0) {
        sys_close(pipefd[1]);
    } else {
        file_close(f1);
    }

    return -1;
    /* (Final) TODO END */
}