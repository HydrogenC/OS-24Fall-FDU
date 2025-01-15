#include <fs/block_device.h>
#include <fs/cache.h>
#include <fs/defines.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <fs/file.h>
#include <common/defines.h>
#include <kernel/printk.h>

// Start block of our filesystem
u64 fs_start = 0;

void init_filesystem() {
    init_block_device();

    char buffer[BLOCK_SIZE];
    block_device.read(0, buffer);

    u32* lba_part_2 = (u32*)&buffer[0x1CE + 0x8];
    u32* numsec_part_2 = (u32*)&buffer[0x1CE + 0xC];

    // printk("LBA of partition 2 is %u. \n", *lba_part_2);
    // printk("Number of sectors of partition 2 is %u. \n", *numsec_part_2);

    const SuperBlock* sblock = get_super_block();
    fs_start = *lba_part_2 + 1;
    block_device.read(fs_start, sblock);
    
    init_bcache(sblock, &block_device);
    init_inodes(sblock, &bcache);
    init_ftable();
}
