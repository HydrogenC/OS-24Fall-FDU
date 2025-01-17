#include <elf.h>
#include <common/string.h>
#include <common/defines.h>
#include <kernel/console.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <kernel/printk.h>
#include <aarch64/trap.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <driver/memlayout.h>

#define STACK_PAGE_COUNT 20
// Leave space on bottom of cmdargs in case that the stack continues popping
#define STACK_BOTTOM_RESERVED 128
#define ALIGN_UP(addr, size) (((usize)(addr) + (size - 1)) & (-size))
#define ALIGN_DOWN(addr, size) (((usize)(addr)) & (-size))

extern int fdalloc(struct file *f);
extern void recycle_proc(Proc *proc);

int execve(const char *path, char *const argv[], char *const envp[])
{
    /* (Final) TODO BEGIN */
    printk("Execve begin\n");

    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode *inode = namei(path, &ctx);

    if (!inode) {
        bcache.end_op(&ctx);
        printk("(warn) program file not found\n");
        return -1;
    }

    inodes.lock(inode);
    Elf64_Ehdr elf_header;
    ASSERT(inodes.read(inode, (u8 *)&elf_header, 0, sizeof(Elf64_Ehdr)) ==
           sizeof(Elf64_Ehdr));

    u32 *elf_magic = (u32 *)(&elf_header.e_ident);
    // Check magic, equivalent to `strcmp`, but I'm too lazy to use that :(
    const char *real_elf_magic = ELFMAG;
    if (*elf_magic != *((u32 *)real_elf_magic)) {
        inodes.unlock(inode);
        inodes.put(&ctx, inode);
        bcache.end_op(&ctx);
        return -1;
    }

    struct pgdir new_pgdir;
    init_pgdir(&new_pgdir);
    init_sections(&new_pgdir.section_head);

    Elf64_Phdr program_header;

    // Assign the position after the last section as the starting point of heap
    u64 heap_start = 0;

    for (u16 ph_index = 0; ph_index < elf_header.e_phnum; ph_index++) {
        u64 offset = elf_header.e_phoff + ph_index * sizeof(Elf64_Phdr);
        struct section *section;

        if (inodes.read(inode, (u8 *)&program_header, offset,
                        sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            printk("(warn) ELF program header read failure\n");
            goto failure;
        }

        // Ignore unloadable sections
        if (program_header.p_type != PT_LOAD) {
            continue;
        }

        if (program_header.p_memsz < program_header.p_filesz) {
            printk("(warn) memsz should not be smaller than filesz\n");
            goto failure;
        }

        section = (struct section *)kalloc(sizeof(struct section));
        section->begin = program_header.p_vaddr;
        section->end = program_header.p_vaddr + program_header.p_memsz;
        section->flags = 0;

        switch (program_header.p_flags) {
        case PF_R | PF_W:
            // RW section
            // section->flags = ST_DATA;
            break;
        case PF_R | PF_X:
            // RO section
            ASSERT(program_header.p_memsz == program_header.p_filesz);
            // section->flags = ST_TEXT;
            break;
        default:
            printk("(warn) unrecognizable section type\n");
            goto failure;
        }

        _insert_into_list(&new_pgdir.section_head, &section->stnode);
        heap_start = MAX(heap_start, section->end);

        // TODO: Use file section lazy loading when possible
        usize bytes_loaded = 0;
        u64 va_pos = section->begin;
        u64 file_offset = program_header.p_offset;
        while (bytes_loaded < program_header.p_filesz) {
            char *new_page = kalloc_page();

            u64 va_page_base = PAGE_BASE(va_pos);
            u64 va_offset_in_page = va_pos - va_page_base;
            u32 read_count = MIN(PAGE_SIZE - va_offset_in_page,
                                 program_header.p_filesz - bytes_loaded);
            read_count = inodes.read(inode, new_page + va_offset_in_page,
                                     file_offset, read_count);
            vmmap(&new_pgdir, va_page_base, new_page, PTE_USER_DATA);

            bytes_loaded += read_count;
            file_offset += read_count;
            va_pos += read_count;

            // If there's BSS after file content, then fill remaining page with zero
            if (bytes_loaded == program_header.p_filesz &&
                bytes_loaded < program_header.p_memsz && va_pos % 4096 != 0) {
                va_offset_in_page = va_pos - PAGE_BASE(va_pos);
                u64 fill_count = PAGE_SIZE - va_offset_in_page;
                memset(new_page + va_offset_in_page, 0, fill_count);
                bytes_loaded += fill_count;
                va_pos += fill_count;
            }
        }

        // BSS section
        if (program_header.p_filesz < program_header.p_memsz) {
            ASSERT(va_pos % PAGE_SIZE == 0);
            while (bytes_loaded < program_header.p_memsz) {
                // Map shared zero page
                vmmap(&new_pgdir, va_pos, get_zero_page(),
                      PTE_USER_DATA | PTE_RO);

                bytes_loaded += PAGE_SIZE;
                va_pos += PAGE_SIZE;
            }
        }

        continue;

failure:
        free_sections(&new_pgdir);
        free_pgdir(&new_pgdir);
        inodes.unlock(inode);
        inodes.put(&ctx, inode);
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.unlock(inode);
    inodes.put(&ctx, inode);
    bcache.end_op(&ctx);

    heap_start = ALIGN_UP(heap_start, PAGE_SIZE);

    struct section *heap_section =
            (struct section *)kalloc(sizeof(struct section));
    // Heap with size 0
    heap_section->flags = ST_HEAP;
    heap_section->begin = heap_start;
    heap_section->end = heap_section->begin;
    heap_section->fp = NULL;
    _insert_into_list(&new_pgdir.section_head, &heap_section->stnode);

    struct section *stack_section =
            (struct section *)kalloc(sizeof(struct section));
    // Initialize a 80k heap
    stack_section->flags = ST_STACK;
    stack_section->end = PHYSTOP;
    stack_section->begin = stack_section->end - STACK_PAGE_COUNT * PAGE_SIZE;
    stack_section->fp = NULL;
    _insert_into_list(&new_pgdir.section_head, &stack_section->stnode);

    // Count arg and envvar size, estimate the initial stack size required
    u64 argc = 0, envc = 0;
    // Size of argc
    u64 strings_size = 0;
    if (argv) {
        while (argv[argc]) {
            strings_size += strlen(argv[argc++]) + 1;
        }
    }

    if (envp) {
        while (envp[envc]) {
            strings_size += strlen(envp[envc++]) + 1;
        }
    }

    // Total size of pointers
    // argc + argv (len = argc+1) + envp (len = envc+1)
    const u64 pointer_size = (1 + (argc + 1) + (envc + 1)) * sizeof(u64);

    // Padding between pointers and strings to ensure that sp is 16-aligned
    u64 padding_size = ALIGN_UP(strings_size, 8) - strings_size;
    if ((strings_size + padding_size + pointer_size) % 16 != 0) {
        padding_size += 8;
        ASSERT((strings_size + padding_size + pointer_size) % 16 == 0);
    }

    const u64 zero = 0;
    // Start position of storing strings
    u64 strings_pos = stack_section->end - STACK_BOTTOM_RESERVED;
    // Put sp under string data, align to 8
    u64 sp = strings_pos - strings_size - padding_size;

    // envp[envc]
    sp -= sizeof(char *);
    copyout(&new_pgdir, (void *)sp, (void *)(&zero), sizeof(char *));

    // Copy envp
    for (i64 i = envc - 1; i >= 0; i--) {
        /* code */
        u32 len = strlen(envp[i]) + 1;
        sp -= sizeof(char *);
        strings_pos -= len;
        copyout(&new_pgdir, (void *)strings_pos, (void *)envp[i], len);
        copyout(&new_pgdir, (void *)sp, (void *)(&strings_pos), sizeof(char *));
    }

    // argv[argc]
    sp -= sizeof(char *);
    copyout(&new_pgdir, (void *)sp, (void *)(&zero), sizeof(char *));

    // Copy args
    for (i64 i = argc - 1; i >= 0; i--) {
        /* code */
        u32 len = strlen(argv[i]) + 1;
        sp -= sizeof(char *);
        strings_pos -= len;
        copyout(&new_pgdir, (void *)strings_pos, (void *)argv[i], len);
        copyout(&new_pgdir, (void *)sp, (void *)(&strings_pos), sizeof(char *));
    }

    // Copy argc
    sp -= sizeof(argc);
    copyout(&new_pgdir, (void *)sp, (void *)(&argc), sizeof(argc));

    ASSERT(sp == stack_section->end - STACK_BOTTOM_RESERVED - pointer_size -
                         strings_size - padding_size);
    ASSERT(sp % 16 == 0);

    Proc *this = thisproc();
    this->ucontext->sp = sp;
    this->ucontext->elr = elf_header.e_entry;

    free_sections(&this->pgdir);
    free_pgdir(&this->pgdir);

    // Migrate page table and sections
    memcpy(&this->pgdir, &new_pgdir, sizeof(struct pgdir));
    _insert_into_list(&new_pgdir.section_head, &this->pgdir.section_head);
    _detach_from_list(&new_pgdir.section_head);
    attach_pgdir(&this->pgdir);

    printk("Execve finished\n");
    return 0;
    /* (Final) TODO END */
}
