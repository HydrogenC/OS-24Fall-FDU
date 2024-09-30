#pragma once

#include <kernel/proc.h>
#include <common/rbtree.h>

#define NCPU 4

struct KernelContext;

struct sched {
    // TODO: customize your sched info
    struct Proc* this_proc;
    struct Proc* idle_proc;
};

struct cpu {
    bool online;
    struct rb_root_ timer;
    struct sched sched;
};

extern struct cpu cpus[NCPU];

void set_cpu_on();
void set_cpu_off();
