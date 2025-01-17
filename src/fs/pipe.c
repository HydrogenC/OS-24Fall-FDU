#include <kernel/mem.h>
#include <kernel/sched.h>
#include <fs/pipe.h>
#include <fs/file.h>
#include <common/string.h>
#include <kernel/printk.h>

void init_pipe(Pipe *pi)
{
    /* (Final) TODO BEGIN */
    pi->nread = 0;
    pi->nwrite = 0;
    pi->readopen = true;
    pi->writeopen = true;

    init_spinlock(&pi->lock);
    init_sem(&pi->rlock, 0);
    init_sem(&pi->wlock, 0);
    /* (Final) TODO END */
}

void init_read_pipe(File *readp, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    readp->readable = true;
    readp->writable = false;
    readp->type = FD_PIPE;
    readp->pipe = pipe;
    readp->off = 0;
    /* (Final) TODO END */
}

void init_write_pipe(File *writep, Pipe *pipe)
{
    /* (Final) TODO BEGIN */
    writep->readable = false;
    writep->writable = true;
    writep->type = FD_PIPE;
    writep->pipe = pipe;
    writep->off = 0;
    /* (Final) TODO END */
}

int pipe_alloc(File **f0, File **f1)
{
    /* (Final) TODO BEGIN */
    *f0 = file_alloc();
    *f1 = file_alloc();
    if (*f0 == NULL || *f1 == NULL) {
        return -1;
    }

    Pipe *pipe = (Pipe *)kalloc(sizeof(Pipe));

    if(!pipe){
        file_close(*f0);
        file_close(*f1);
        return -1;
    }

    init_pipe(pipe);
    init_read_pipe(*f0, pipe);
    init_write_pipe(*f1, pipe);

    return 0;
    /* (Final) TODO END */
}

void pipe_close(Pipe *pi, int writable)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);
    if (writable) {
        pi->writeopen = false;
        post_all_sem(&pi->rlock);
    } else {
        pi->readopen = false;
        post_all_sem(&pi->wlock);
    }

    if (!pi->readopen && !pi->writeopen) {
        release_spinlock(&pi->lock);
        kfree(pi);
        return;
    }

    release_spinlock(&pi->lock);
    /* (Final) TODO END */
}

int pipe_write(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);

    if(!pi->writeopen){
        release_spinlock(&pi->lock);
        return -1;
    }

    int count = 0;
    while (count < n) {
        // Already full
        while (pi->nwrite == pi->nread + PIPE_SIZE) {
            release_spinlock(&pi->lock);
            if (!pi->readopen || !wait_sem(&pi->wlock)) {
                // Process already killed or read pipe closed
                return -1;
            }
            acquire_spinlock(&pi->lock);
        }

        pi->data[pi->nwrite++ % PIPE_SIZE] = *(char *)(addr + count);
        count++;
    }

    // Wake up reader
    post_all_sem(&pi->rlock);
    release_spinlock(&pi->lock);

    return count;
    /* (Final) TODO END */
}

int pipe_read(Pipe *pi, u64 addr, int n)
{
    /* (Final) TODO BEGIN */
    acquire_spinlock(&pi->lock);

    if(!pi->readopen){
        release_spinlock(&pi->lock);
        return -1;
    }

    // Wait for pending data
    while (pi->nwrite == pi->nread && pi->writeopen) {
        release_spinlock(&pi->lock);
        if (!wait_sem(&pi->rlock)) {
            // Process already killed or read pipe closed
            return -1;
        }
        acquire_spinlock(&pi->lock);
    }

    int count = 0;
    while (count < n) {
        if (pi->nread == pi->nwrite) {
            break;
        }

        *(char *)(addr + count) = pi->data[pi->nread++ % PIPE_SIZE];
        count++;
    }

    // Wake up reader
    post_all_sem(&pi->wlock);
    release_spinlock(&pi->lock);

    return count;
    /* (Final) TODO END */
}