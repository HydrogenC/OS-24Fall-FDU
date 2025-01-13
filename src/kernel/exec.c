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
#include <aarch64/trap.h>
#include <fs/file.h>
#include <fs/inode.h>

extern int fdalloc(struct file *f);

int execve(const char *path, char *const argv[], char *const envp[])
{
    /* (Final) TODO BEGIN */
    OpContext ctx;
    bcache.begin_op(&ctx);
    Inode *inode = namei(&ctx, path);

    if (!inode) {
        bcache.end_op(&ctx);
        return -1;
    }

    inodes.lock(inode);
    Elf64_Ehdr elf_header;
    ASSERT(inodes.read(inode, &elf_header, 0, sizeof(Elf64_Ehdr)) ==
           sizeof(Elf64_Ehdr));

    // Check magic
    u32 *elf_magic = (u32 *)(&elf_header.e_ident);
    if (*elf_magic != *((u32 *)ELFMAG)) {
        inodes.unlock(inode);
        inodes.put(&ctx, inode);
        bcache.end_op(&ctx);
        return -1;
    }

    Proc *proc = create_proc();

    Elf64_Phdr program_header;
    for (u16 ph_index = 0; ph_index < elf_header.e_phnum; ph_index++) {
        u64 offset = elf_header.e_phoff + ph_index * sizeof(Elf64_Phdr);
        ASSERT(inodes.read(inode, &program_header, offset,
                           sizeof(Elf64_Phdr)) == sizeof(Elf64_Phdr));

        if (program_header.p_type != PT_LOAD) {
            continue;
        }

        ASSERT(program_header.p_memsz >= program_header.p_filesz);
        struct section *section =
                (struct section *)kalloc(sizeof(struct section));
        section->begin = program_header.p_vaddr;
        section->end = program_header.p_vaddr + program_header.p_memsz;
        section->flags = 0;

        _insert_into_list(&proc->pgdir.section_head, section);
    }

    inodes.unlock(inode);
    inodes.put(&ctx, inode);
    bcache.end_op(&ctx);

    /* (Final) TODO END */
}
