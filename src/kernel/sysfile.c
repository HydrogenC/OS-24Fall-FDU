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
    for (usize i = 0; i < NFILE_PROC; i++) {
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

#define ALIGN_UP(addr, size) (((usize)(addr) + (size - 1)) & (-size))
#define ALIGN_DOWN(addr, size) (((usize)(addr)) & (-size))

define_syscall(mmap, void *addr, int length, int prot, int flags, int fd,
               int offset)
{
    /* (Final) TODO BEGIN */
    File *f = fd2file(fd);

    if (!f) {
        printk("(warn) mmap: file doesn't exist! \n");
        return -1;
    }

    // Check permission
    if ((prot & PROT_WRITE) && flags != MAP_PRIVATE && !f->writable) {
        printk("(warn) mmap: creating shared writable mmap but file isn't writable! \n");
        return -1;
    }

    Proc *this = thisproc();

    acquire_spinlock(&this->pgdir.lock);
    u64 begin, end;
    if (!addr) {
        // Start to search from 0x70000000, which is between heap and stack
        bool valid = false;
        begin = 0x70000000;
        end = begin + length;

        // Find unoccupied memory area
        while (!valid) {
            valid = true;
            ListNode *node = this->pgdir.section_head.next;
            while (node != &this->pgdir.section_head) {
                struct section *section =
                        container_of(node, struct section, stnode);
                if (section->begin < end && section->end > begin) {
                    begin = ALIGN_UP(section->end, PAGE_SIZE);
                    end = begin + length;
                    valid = false;
                    break;
                }

                node = node->next;
            }
        }

        if (!valid) {
            release_spinlock(&this->pgdir.lock);
            printk("(warn) cannot find appropriate space for mmap\n");
            return -1;
        }
    } else {
        begin = (u64)addr;
        end = begin + length;

        ListNode *node = this->pgdir.section_head.next;
        while (node != &this->pgdir.section_head) {
            struct section *section =
                    container_of(node, struct section, stnode);
            if (section->begin < end && section->end > begin) {
                release_spinlock(&this->pgdir.lock);
                printk("(warn) given address invalid since it intersects with existing sections\n");
                return -1;
            }
        }
    }

    printk("Mapping file to %llu - %llu\n", begin, end);
    struct section *map_section =
            (struct section *)kalloc(sizeof(struct section));

    map_section->begin = begin;
    map_section->end = end;
    map_section->flags =
            (flags == MAP_PRIVATE ? ST_MMAP_PRIVATE : ST_MMAP_SHARED);
    map_section->fp = file_dup(f);
    map_section->offset = offset;
    map_section->length = length;
    map_section->prot = prot;

    _insert_into_list(&this->pgdir.section_head, &map_section->stnode);
    release_spinlock(&this->pgdir.lock);

    return begin;
    /* (Final) TODO END */
}

define_syscall(munmap, void *addr, size_t length)
{
    /* (Final) TODO BEGIN */
    Proc *this = thisproc();
    acquire_spinlock(&this->pgdir.lock);

    // Find unoccupied memory area
    struct section *mapped_section = NULL;
    ListNode *node = this->pgdir.section_head.next;
    while (node != &this->pgdir.section_head) {
        struct section *section = container_of(node, struct section, stnode);
        if (section->begin == (u64)addr) {
            mapped_section = section;
            break;
        }

        node = node->next;
    }

    if (!mapped_section || !mapped_section->fp) {
        // No effect if mapping doesn't exist
        release_spinlock(&this->pgdir.lock);
        return 0;
    }

    bool free_whole_section = false;
    if (length >= mapped_section->end - mapped_section->begin) {
        length = mapped_section->end - mapped_section->begin;
        free_whole_section = true;
    }

    // Only write back public mappings
    if (mapped_section->flags == ST_MMAP_SHARED &&
        (mapped_section->prot & PROT_WRITE)) {
        write_back(&this->pgdir, mapped_section->fp, mapped_section->begin,
                   mapped_section->offset, length);
    }

    u64 va = ALIGN_DOWN(mapped_section->begin, PAGE_SIZE);
    if (free_whole_section) {
        while (va < mapped_section->end) {
            PTEntriesPtr pte = get_pte(&this->pgdir, va, false);
            if (!pte) {
                continue;
            }

            if (CHECK_DESCRIPTOR(*pte)) {
                void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
                kfree_page(old_page);
            }

            *pte = 0;
            va += PAGE_SIZE;
        }

        _detach_from_list(&mapped_section->stnode);
        file_close(mapped_section->fp);
        kfree(mapped_section);
    } else {
        while (va + PAGE_SIZE <= mapped_section->begin + length) {
            PTEntriesPtr pte = get_pte(&this->pgdir, va, false);
            if (!pte) {
                continue;
            }

            if (CHECK_DESCRIPTOR(*pte)) {
                void *old_page = (void *)P2K(PTE_ADDRESS(*pte));
                kfree_page(old_page);
            }

            *pte = 0;
            va += PAGE_SIZE;
        }

        mapped_section->begin += length;
        mapped_section->offset += length;
        mapped_section->length -= length;
    }

    arch_tlbi_vmalle1is();
    release_spinlock(&this->pgdir.lock);
    return 0;
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
    usize index;
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

    usize inumber = inodes.lookup(dp, name, &index);
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
    if (inodes.write(&ctx, dp, (u8 *)&de, sizeof(de) * index, sizeof(de)) !=
        sizeof(de))
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
        if (type == target->entry.type) {
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

    target->entry.type = type;
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

        // We do not increment ref to self again for `.` to avoid circular ref
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
    f->off = (omode & O_APPEND) ? ip->entry.num_bytes : 0;
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

define_syscall(mknodat, int dirfd, const char *path,
               __attribute__((unused)) mode_t mode, dev_t dev)
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
        inodes.unlock(inode);
        inodes.put(&ctx, inode);
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

define_syscall(pipe2, int pipefd[2], __attribute__((unused)) int flags)
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