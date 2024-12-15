#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include "fs-sim.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>


static char mounted_disk[64] = {0};
static int cwd = 0;
static Superblock superblock;
static uint8_t buffer[1024] = {0};
static int global_fd = -1;
static int fs_mounted = 0;

static int write_superblock() {
    if (lseek(global_fd, 0, SEEK_SET) < 0) return -1;
    if (write(global_fd, &superblock, sizeof(Superblock)) != sizeof(Superblock)) return -1;
    return 0;
}

static void recursive_delete(int i) {
    if (superblock.inode[i].dir_parent & 0x80) {
        // Delete children
        for (int j = 0; j < 126; j++) {
            if ((superblock.inode[j].used_size & 0x80) && (superblock.inode[j].dir_parent & 0x7F)==i) {
                recursive_delete(j);
            }
        }
    } else {
        int size = superblock.inode[i].used_size & 0x7F;
        int start = superblock.inode[i].start_block;
        for (int j = start; j < start + size; j++) {
            //printf("Delete: free %d\n", j);
            superblock.free_block_list[j / 8] &= ~(1 << (7 - (j % 8)));
            uint8_t zeroes[1024] = {0};
            memset(zeroes, 0, 1024);
            lseek(global_fd, j * 1024, SEEK_SET);
            write(global_fd, zeroes, 1024);
        }
    }
    memset(&superblock.inode[i], 0, sizeof(Inode));
}

/**
 * Check and save a reference of the virtual disk (disk0)
 * 
 * Load the superblock of the FS, by reading the virtual disk file.
 * 
 * Do the following consistency checks in order:
 * 1. If the state of an inode is free, then all bits in this inode must be zero. Otherwise, the name attribute
 * stored in the inode must have at least one bit that is not zero.
 * 2. The start block of every inode that is in use and pertains to a file (i.e. when the directory bit is not
 * set) must have a value between 1 and 127 inclusive. Moreover, the size of every inode that is in use
 * and pertains to a file must be such that its last block is also between 1 and 127.
 * 3. The size and start block of an inode pertaining to a directory (i.e. the directory bit is set) must
 * be zero.
 * 4. For every inode that is in use, the index of its parent inode cannot be 126. Moreover, if the index of
 * the parent inode is between 0 and 125 inclusive, then the parent inode must be in use and marked as a
 * directory.
 * 5. The name of every file/directory must be unique in each directory (not in the entire file system)
 * 6. Blocks that are marked free in the free-space list cannot be allocated to any file. Similarly, blocks that
 * are marked in use in the free-space list must be allocated to exactly one file.
 * 
 * If the file system is inconsistent, you must print the following error message to stderr:
 * Error: File system in <disk name> is inconsistent (error code: <number>)
 * 
 * If the FFD passes the test, then mount it into the program by reading the superblock of this FFD into 
 * the memory, and unmount the previous FFD. Otherwise, keep using the last FFD.
 * 
 * A success fs_mount will change the current working directory to root
 */
void fs_mount(char *new_disk_name){
    int fd = open(new_disk_name, O_RDWR);
    if (fd < 0){
        fprintf(stderr, "Error: Cannot find disk %s\n", new_disk_name);
        close(fd);
        return;
    }
    Superblock sb;
    lseek(fd, 0, SEEK_SET);
    read(fd, &sb, sizeof(Superblock));

    int prev_disk_fd = global_fd;
    Superblock prev_superblock;
    memcpy(&prev_superblock, &superblock, sizeof(Superblock));
    global_fd = fd;
    memcpy(&superblock, &sb, sizeof(Superblock));

    // Check 1
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        uint8_t used_size = inode->used_size;
        if (used_size <= 127) { // inode is free
            uint8_t *inode_bytes = (uint8_t *)inode;
            for (int j = 0; j < sizeof(Inode); j++) {
                if (inode_bytes[j] != 0) {
                    close(fd);
                    global_fd = prev_disk_fd;
                    superblock = prev_superblock;
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 1)\n", new_disk_name);
                    return;
                }
            }
        } else { // inode is in use
            uint8_t *inode_bytes = (uint8_t *)inode;
            int not_zero = 0;
            for (int j = 0; j < sizeof(Inode); j++) {
                if (inode_bytes[j] != 0) {
                    not_zero++;
                }
            }
            if (not_zero == 0) {
                close(fd);
                global_fd = prev_disk_fd;
                superblock = prev_superblock;
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 1)\n", new_disk_name);
                return;
            }
        }
    }    
    

    // Check 2
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        uint8_t used_size = inode->used_size;
        uint8_t dir_parent = inode->dir_parent;
        if(used_size > 127) { // inode is in use
            if (dir_parent <= 127) { // inode is a file
                uint8_t size = inode->used_size & 0x7F;
                uint8_t start_block = inode->start_block;
                if((start_block < 1) || (start_block > 127)){
                    close(fd);
                    global_fd = prev_disk_fd;
                    superblock = prev_superblock;
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                    return;
                }
                if (start_block + size - 1 > 127) {
                    close(fd);
                    global_fd = prev_disk_fd;
                    superblock = prev_superblock;
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 2)\n", new_disk_name);
                    return;
                }
            }
        }
    }

    // Check 3
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        uint8_t dir_parent = inode->dir_parent;
        if (dir_parent > 127) { // inode is a directory
            uint8_t start_block = inode->start_block;
            uint8_t used_size = inode->used_size;
            if ((start_block != 0) || (used_size != 0 && used_size != 128)) {
                close(fd);
                global_fd = prev_disk_fd;
                superblock = prev_superblock;
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 3)\n", new_disk_name);
                return;
            }
        }
    }

    // Check 4
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        uint8_t used_size = inode->used_size;
        if (used_size > 127) { // inode is in use
            uint8_t parent = inode->dir_parent & 0x7F;
            if (parent == 126){
                close(fd);
                global_fd = prev_disk_fd;
                superblock = prev_superblock;
                fprintf(stderr, "Error: File system in %s is inconsistent (error code: 4)\n", new_disk_name);
                return;
            }
            else if (parent >= 0 && parent <= 125){
                Inode *parent_inode = &superblock.inode[parent];
                uint8_t used_parent = parent_inode->used_size >> 7;
                uint8_t directory_parent = parent_inode->dir_parent >> 7;
                if (directory_parent == 0 || used_parent == 0){
                    close(fd);
                    global_fd = prev_disk_fd;
                    superblock = prev_superblock;
                    fprintf(stderr, "Error: File system in %s is inconsistent (error code: 4)\n", new_disk_name);
                    return;
                }
            }
        }
    }

    // Check 5
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        uint8_t used = inode->used_size >> 7;
        if (used == 1) {
            uint8_t parent = inode->dir_parent & 0x7F;
            for (int j = i + 1; j < 126; j++) {
                Inode *inode2 = &superblock.inode[j];
                uint8_t inode2_used = inode2->used_size >> 7;
                if (inode2_used == 1) {
                    uint8_t parent2 = inode2->dir_parent & 0x7F;
                    if (parent == parent2 && strncmp(inode->name, inode2->name, 5) == 0) {
                        close(fd);
                        global_fd = prev_disk_fd;
                        superblock = prev_superblock;
                        fprintf(stderr, "Error: File system in %s is inconsistent (error code: 5)\n", new_disk_name);
                        return;
                    }
                }
            }
        }
    }

    // Check 6
    int block_usage[128] = {0};
    for (int i = 0; i < 126; i++) {
        if (!(superblock.inode[i].used_size & 0x80)) continue;
        if (!(superblock.inode[i].dir_parent & 0x80)) {
            int start = superblock.inode[i].start_block;
            int size = superblock.inode[i].used_size & 0x7F;
            for (int b = start; b < start + size; b++) {
                block_usage[b]++;
            }
        }
    }
    for (int b = 1; b < 128; b++) {
        if (((superblock.free_block_list[(b) / 8]>> (7 - (b % 8))) & 1) && block_usage[b] != 1){
            fprintf(stderr, "Error: File system in %s is inconsistent (error code: 6)\n", new_disk_name);
        }
        if (!((superblock.free_block_list[(b) / 8]>> (7 - (b % 8))) & 1) && block_usage[b] != 0){
            fprintf(stderr, "Error: File system in %s is inconsistent (error code: 5)\n", new_disk_name);
        }
    }

    // no inconsistencies
    // mount
    fs_mounted = 1;
    if (prev_disk_fd>=0) {
        close(prev_disk_fd);
    }

    memset(buffer, 0, sizeof(buffer));
    memset(mounted_disk, 0, sizeof(mounted_disk));
    strcpy(mounted_disk, new_disk_name);
    //printf("Debug mount: mounted disk %s\n", mounted_disk);
    cwd = 0;
    
}

/**
 * Create a file on current mounted FFD with given name and a fixed size.
 * 
 * When assigning blocks to the file, the file must be allocated a number of contiguous blocks.
 * 
 * A size of 0 means the user wants to create a directory.
 * 
 * Remember that you cannot have two files with the same name under the same path.
 * 
 * Name . and .. are reserved and cannot be used.
 * 
 * In this assignment, user can have symbols in the name as well (such as ...). We will only test the
 * characters that is on your keyboard (i.e. will not test ‘\n’, ‘\r’, etc.).
 */
void fs_create(char name[5], int size){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    int free_inode_index = -1;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) == 0) { // Free inode
            free_inode_index = i;
            break;
        }
    }
    if (free_inode_index == -1) {
        fprintf(stderr, "Error: Superblock in disk %s is full, cannot create %s\n", mounted_disk, name);
        return;
    }

    int pd = (cwd == 0) ? 127 : cwd;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) != 0 && (inode->dir_parent & 0x7F) == pd) { 
            if (strncmp(inode->name, name, 5) == 0) { 
                fprintf(stderr, "Error: File or directory %s already exists\n", name);
                return;
            }
        }
    }
    if (strncmp(name, ".", 5) == 0 || strncmp(name, "..", 5) == 0) {
        fprintf(stderr, "Error: File or directory %s already exists\n", name);
        return;
    }


    int start_block = -1, count = 0;
    for (int i = 1; i < 128; i++) {
        int bit = (superblock.free_block_list[i / 8] >> (7 - (i % 8))) & 1;
        if (bit == 0) {
            if (count == 0) start_block = i;
            count++;
            if (count == size) break;
        } else {
            count = 0;
        }
    }
    if (count < size) {
        //printf("Debug create: disk name: %s\n", mounted_disk);
        fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n", size, mounted_disk);
        return;
    }

    for (int i = start_block; i < start_block + size; i++) {
        superblock.free_block_list[i / 8] |= (1 << (7 - (i % 8)));
    }

    Inode *new_inode = &superblock.inode[free_inode_index];
    memset(new_inode, 0, sizeof(Inode));
    strncpy(new_inode->name, name, 5);
    new_inode->used_size = (0x80 | size);
    new_inode->start_block = (size > 0) ? start_block : 0;
    if (size == 0){
        new_inode->dir_parent = 0x80 | ((cwd == 0 ? 127 : cwd) & 0x7F); 
    }
    else {
        new_inode->dir_parent = ((cwd == 0 ? 127 : cwd) & 0x7F); 
    }

    write_superblock();
    //printf("Create: inode: %d, size: %d, dir_parent: %d, name: %5s\n", free_inode_index, 
    //new_inode -> used_size, new_inode -> dir_parent, name);
}


/**
 * Delete the file on mounted FFD by its name.
 * 
 * If a directory is selected, then remove the directory and all files/directories in it.
 * 
 * Deletes the file or directory with the given name in the current working directory. If the name represents a
 * directory, your program should recursively delete all files and directories within this directory. For every file
 * or directory that is deleted, you must zero out the corresponding inode and data block(s). Do not shift other
 * inodes or file data blocks after deletion. If the specified file or directory is not found in the current working
 * directory, print the following error message to stderr:
 * Error: File or directory <file name> does not exist
 */
void fs_delete(char name[5]){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    int target_index = -1;
    int pd = (cwd == 0) ? 127 : cwd;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) != 0 && 
            (inode->dir_parent & 0x7F) == pd && strncmp(inode->name, name, 5) == 0) { 
            target_index = i;
            break;
        }
    }
    if (target_index == -1) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }
    recursive_delete(target_index);
    write_superblock();
}



/**
 * Put 1 KB data in the specified block to buffer.
 *
 * Opens the file with the given name and reads the block num-th block of the file into the buffer. If no such
 * file exists or the given name corresponds to a directory under the current working directory, the following
 * error must be printed to stderr:
 * Error: File <file name> does not exist
 * 
 * If the block num is not in the range of [0, size-1], where size is the number of blocks allocated to the
 * file, print the following error message to stderr:
 * Error: <file name> does not have block <block_num> 
 */
 void fs_read(char name[5], int block_num){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    int file_index = -1;
    int pd = (cwd == 0) ? 127 : cwd;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) != 0 && 
            (inode->dir_parent & 0x7F) == pd && 
            strncmp(inode->name, name, 5) == 0 && 
            (inode->dir_parent & 0x80) == 0) { 
            file_index = i;
            break;
        }
    }
    if (file_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *file_inode = &superblock.inode[file_index];
    uint8_t file_size = file_inode->used_size & 0x7F;
    uint8_t start = file_inode->start_block;

    if (block_num < 0 || block_num >= file_size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }
    lseek(global_fd, (start + block_num ) * 1024, SEEK_SET);
    read(global_fd, buffer, 1024);


}


/**
 * Put 1 KB data in buffer to the specified block.
 * 
 * Opens the file with the given name and writes the content of the buffer to the block num-th block of the
 * file. If no such file exists or the given name corresponds to a directory under the current working directory,
 * the following error must be printed to stderr:
 * Error: File <file name> does not exist
 * 
 * If the block num is not in the range of [0, size-1], where size is the number of blocks allocated to the
 * file, print the following error message to stderr:
 * Error: <file name> does not have block <block_num>
 */
void fs_write(char name[5], int block_num){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    if (buffer == NULL) {
        fprintf(stderr, "Error: Buffer is not initialized\n");
        return;
    }
    int file_index = -1;
    int pd = (cwd == 0) ? 127 : cwd;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80) != 0 && 
            (inode->dir_parent & 0x7F) == pd && 
            strncmp(inode->name, name, 5) == 0 && 
            (inode->dir_parent & 0x80) == 0) { 
            file_index = i;
            break;
        }
    }
    if (file_index == -1) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    Inode *file_inode = &superblock.inode[file_index];
    uint8_t file_size = file_inode->used_size & 0x7F; 

    if (block_num < 0 || block_num >= file_size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }

    //printf("Write: buffer %s\n", buffer);
    int start = file_inode->start_block;

    lseek(global_fd, (start + block_num) * 1024, SEEK_SET);
    write(global_fd, buffer, 1024);

    write_superblock();
}


/**
 * Flushes the buffer by zeroing it and writes the new bytes into the buffer. 
 * Error handling is not handled in this function.
 */
void fs_buff(char buff[1024]){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    memset(buffer, 0, 1024);
    int len = strlen((char*)buff);
    memcpy(buffer, buff, len);
}


/**
 * Same thing as when you type ls in your terminal.
 * List files and directories based on the order they stored in inodes.
 * Lists all files and directories that exist in the current directory, including the special directories . and ..
 * which represent the current working directory and the parent directory of the current working directory, respectively. 
 * 
 * You must print the result to stdout. 
 * Always print . and .. as the first and second rows of the result, respectively, and then all files and directories 
 * according to the indices of their corresponding inodes. 
 * 
 * For files, you must print the size of the file as well. For directories, you must show the number of 
 * files and directories that exist in this directory (do not do this recursively). If the current directory is the root
 * directory of the virtual disk, then .. and . both represent the current directory.
 */
void fs_ls(void){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Determine the current directory's parent directory
    int parent_of_cwd;
    if (cwd == 0) {
        parent_of_cwd = 127;
    } else {
        // Get the parent directory from the inode of cwd
        parent_of_cwd = superblock.inode[cwd].dir_parent & 0x7F;
    }

    // Set pd based on cwd
    int pd = (cwd == 0) ? 127 : cwd;

    int num_files_in_cwd = 0, num_files_in_parent = 0;
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        if ((inode->used_size & 0x80)) { // Check if inode is in use
            int inode_parent = inode->dir_parent & 0x7F;

            // Count entries in the current working directory
            if (inode_parent == pd) {
                num_files_in_cwd++;
            }

            // Count entries in the parent directory
            if (inode_parent == parent_of_cwd) {
                num_files_in_parent++;
            }
        }
    }

    // Print . and .. with their respective counts
    printf(".   %5d\n", num_files_in_cwd + 2); // Including . and potentially hidden entries
    if (cwd == 0){
        printf("..  %5d\n", num_files_in_cwd + 2); // Root's parent is itself
    }
    else{
        printf("..  %5d\n", num_files_in_parent + 2); // Including .. and potentially hidden entries
    }



    // List all files and directories in the current directory
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        char name[6];
        memcpy(name, superblock.inode[i].name, 5);
        name[5] = '\0';
        // Check if the inode is in use and belongs to the current working directory
        if ((inode->used_size & 0x80) && (inode->dir_parent & 0x7F) == pd) {
            if (inode->dir_parent & 0x80) { // Directory
                int num_children = 0;

                // Count children in this directory
                for (int j = 0; j < 126; j++) {
                    if ((superblock.inode[j].used_size & 0x80) &&
                        (superblock.inode[j].dir_parent & 0x7F) == i) {
                        num_children++;
                    }
                }

                printf("%-5s %3d\n", name, num_children + 2);
            } else { // File
                uint8_t file_size = inode->used_size & 0x7F; // Extract file size
                printf("%-5s %3d KB\n", name, file_size);
            }
        }
    }
}


/**
 * Changes the size of the file with the given name to new size.
 * If no such file exists in the current working directory or the name corresponds to a directory rather than a file, 
 * your program should print the following error message to stderr:
 * Error: File <file name> does not exist
 *
 * If the new size is greater than the current size of the file, you need to allocate more blocks to this file.
 * Keep the start block fixed and add new data blocks to the end such that the file’s data blocks are still contiguous. 
 * If there are enough free blocks after the last block of this file, change the size in the inode to new size.
 * 
 * Otherwise, try moving the file so that you can increase the file size to the new size,
 * copy all file data from the old blocks to the new blocks, and zero out the old blocks. 
 * 
 * The new starting block must be at the first available one that has enough space for the resized file. 
 * If there are not enough contiguous free blocks on the disk to fit the file with its new size, print the following 
 * error message to stderr and do not update the file size:
 * Error: File <file name> cannot expand to size <new size>
 * 
 * If the new size is smaller than the current size, then delete and zero out blocks from the tail of the block
 * sequence allocated to this file. 
 * The starting block is not moved when decreasing the size of a file. 
 * 
 * Finally, change the size attribute in the inode to the new size. 
 * 
 * You can assume that the second argument of this function, i.e. new size, is always greater than zero.
 */
void fs_resize(char name[5], int new_size){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    Inode *file_inode = NULL;
    int pd = (cwd == 0) ? 127 : cwd;
    
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && // In use
            !(superblock.inode[i].dir_parent & 0x80) && // Not a directory
            (superblock.inode[i].dir_parent & 0x7F) == pd && // In the current directory
            strncmp(superblock.inode[i].name, name, 5) == 0) { // Name matches
            file_inode = &superblock.inode[i];
            break;
        }
    }

    if (!file_inode) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }

    int current_size = file_inode->used_size & 0x7F; // Extract current size
    int start_block = file_inode->start_block;

    // If increasing the size
    if (new_size > current_size) {
        int additional_blocks_needed = new_size - current_size;
        int free_space_contiguous = 0;

        // Check if there are enough contiguous blocks after the current file
        for (int i = start_block + current_size; i < 128; i++) {
            if (!(superblock.free_block_list[i / 8] >> (7 - (i % 8))) & 1) {
                free_space_contiguous++;
                if (free_space_contiguous == additional_blocks_needed) {
                    // Update inode and mark blocks as used
                    for (int j = start_block + current_size; j < start_block + new_size; j++) {
                        superblock.free_block_list[j / 8] |= (1 << ( 7 - (j % 8)));
                    }
                    file_inode->used_size = (file_inode->used_size & 0x80) | new_size; // Update size
                    write_superblock();
                    return;
                }
            } else {
                break;
            }
        }

        // Not enough contiguous blocks; try to relocate the file
        free_space_contiguous = 0;
        int new_start_block = -1;

        for (int i = 1; i < 127 - new_size; i++) { // Start from block 1 (superblock is block 0)
            if (!((superblock.free_block_list[i / 8] >> (7 - (i % 8))) & 1)) {
                if (free_space_contiguous == 0) {
                    new_start_block = i;
                }
                free_space_contiguous++;
                if (free_space_contiguous == new_size) {
                    //printf("Resize: index %d\n", new_start_block);
                    // Relocate the file
                    char *temp_buf = malloc(new_size * 1024);
                    
                    for (int j = 0; j < current_size; j++) {
                        // Copy data from old blocks to new blocks
                        lseek(global_fd, (start_block + j) * 1024, SEEK_SET);
                        read(global_fd, temp_buf + j*1024, 1024);
                    }
                    //printf("Resize: buffer %s\n",temp_buf);

                    // Update the free block list: clear old blocks
                    for (int j = start_block; j < start_block + current_size; j++) {
                        //printf("Resize: clear block %d\n", j);
                        superblock.free_block_list[j / 8] &= ~(1 << (7 - (j % 8)));
                        uint8_t zeroes[1024] = {0};
                        memset(zeroes, 0, 1024);
                        lseek(global_fd, j * 1024, SEEK_SET);
                        write(global_fd, zeroes, 1024);
                    }

                    // Mark new blocks as used
                    for (int j = new_start_block; j < new_start_block + current_size; j++) {
                        superblock.free_block_list[j / 8] |= (1 << (7 - (j % 8)));
                        
                    }

                    for (int j = 0; j < current_size; j++) {
                        // Copy data from old blocks to new blocks
                        lseek(global_fd, (j + new_start_block) * 1024, SEEK_SET);
                        write(global_fd, temp_buf + j*1024, 1024);
                    }

                    for (int j = new_start_block + current_size; j < new_start_block - current_size + new_size; j++) {
                        //printf("Resize: free %d\n", j);
                        superblock.free_block_list[j / 8] &= ~(1 << (7 - (j % 8)));
                        uint8_t zeroes[1024] = {0};
                        memset(zeroes, 0, 1024);
                        lseek(global_fd, j * 1024, SEEK_SET);
                        write(global_fd, zeroes, 1024);
                    }

                    // Update inode
                    free(temp_buf);
                    file_inode->start_block = new_start_block;
                    file_inode->used_size = (file_inode->used_size & 0x80) | new_size;
                    write_superblock();
                    return;
                }
            } else {
                free_space_contiguous = 0;
            }
        }

        // If no space is found
        fprintf(stderr, "Error: File %s cannot expand to size %d\n", name, new_size);
        return;
    }

    // If decreasing the size
    if (new_size < current_size) {
        // Zero out the unused blocks
        for (int j = start_block + new_size; j <= start_block + current_size - new_size; j++) {
            //printf("Resize: free %d\n", j);
            superblock.free_block_list[j / 8] &= ~(1 << (7 - (j % 8)));
            uint8_t zeroes[1024] = {0};
            memset(zeroes, 0, 1024);
            lseek(global_fd, j * 1024, SEEK_SET);
            write(global_fd, zeroes, 1024);
        }

        // Update inode
        file_inode->used_size = (file_inode->used_size & 0x80) | new_size;
    }
    write_superblock();
}


/**
* Organize the contents of all files in FFD.
* You should shift all files to the lowest start block index, in the meantime maintain the order of those
* files (i.e., if file1 is located physically after file4, then after defragmentation, file1 should still physically
* come after file4).
* When moving blocks, the data must be transferred to the new blocks and the old blocks must be zeroed out.
* Hence, files will contain the same data as before. 
* Error handling is not handled in this function.
*/
void fs_defrag(void){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    int order[126], count=0;
    for (int i = 0; i < 126; i++) {
        if ((superblock.inode[i].used_size & 0x80) && !(superblock.inode[i].dir_parent & 0x80)) order[count++] = i;
    }
    // Sort by start block
    for (int a = 0; a < count; a++) {
        for (int b = a+1; b < count; b++) {
            if (superblock.inode[order[a]].start_block > superblock.inode[order[b]].start_block) {
                int tmp = order[a]; 
                order[a]=order[b]; 
                order[b]=tmp;
            }
        }
    }
    int next_free_block = 1; // Start after the superblock (block 0)

    // Iterate over inodes, finding files and directories in order of their current start blocks
    for (int i = 0; i < count; i++) {
        int inode_index = order[i];
        Inode *inode = &superblock.inode[inode_index];

        // Check if the inode is in use and represents a file (not a directory)
        int file_size = inode->used_size & 0x7F; // Get the file size in blocks
        int old_start_block = inode->start_block;

        // If the file is already at the correct position, continue
        if (old_start_block == next_free_block) {
            next_free_block += file_size;
            continue;
        }

        char *temp_buf = malloc(file_size * 1024);
                    
        for (int j = 0; j < file_size; j++) {
            // Copy data from old blocks to new blocks
            
            lseek(global_fd, (old_start_block + j) * 1024, SEEK_SET);
            read(global_fd, temp_buf + j* 1024, 1024);
            //printf("Defrag: old block %d, char: %c\n", j + old_start_block, temp_buf[j]);
        }

        //printf("Defrag: buffer %s\n",temp_buf);

        //Update the free block list: clear old blocks
        for (int j = old_start_block; j < old_start_block + file_size; j++) {
            //printf("Defrag: clear block %d\n", j);
            superblock.free_block_list[j / 8] &= ~(1 << (7 - (j % 8)));
            uint8_t zeroes[1024] = {0};
            memset(zeroes, 0, 1024);
            lseek(global_fd, j * 1024, SEEK_SET);
            write(global_fd, zeroes, 1024);
        }

        //Mark new blocks as used
        for (int j = next_free_block; j < next_free_block + file_size; j++) {
            superblock.free_block_list[j / 8] |= (1 << (7 - (j % 8)));
            
        }

        for (int j = 0; j < file_size; j++) {
            // Copy data from old blocks to new blocks
            //printf("Defrag: copy block %d\n", j + next_free_block);
            lseek(global_fd, (j + next_free_block) * 1024, SEEK_SET);
            write(global_fd, temp_buf + j*1024, 1024);
        }

        // Update the inode's start block
        inode->start_block = next_free_block;
        free(temp_buf);

        // Advance the next free block pointer
        next_free_block += file_size;
    }
    
    write_superblock();
}


/**
* Changes the current working directory to a directory with the specified name in the current working directory.
* This directory can be ., .., or any directory that the user created on the virtual disk. 
* 
* If this directory does not exist in the current working directory, i.e., the name does not exist or it points to a file rather than a
* directory, print the following error message to stderr:
* Error: Directory <directory name> does not exist
* 
* You can assume that the given name does not contain a slash or backslash character
*/
void fs_cd(char name[5]){
    if (!fs_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    // Handle special case for '.' (current directory)
    if (strncmp(name, ".", 5) == 0) {
        return; // No change needed
    }

    // Handle special case for '..' (parent directory)
    if (strncmp(name, "..", 5) == 0) {
        if (cwd == 0) {
            // Already in the root directory, no change
            return;
        }
        // Set cwd to the parent directory
        int p = superblock.inode[cwd].dir_parent & 0x7F;
        if (p ==127){
            cwd = 0;
        }
        else{
            cwd = p;
        }
        //printf("Debug 1: %i\n", cwd);
        return;
    }

    // Search for the directory with the given name in the current working directory
    for (int i = 0; i < 126; i++) {
        Inode *inode = &superblock.inode[i];
        int pd = (cwd == 0) ? 127 : cwd;

        // Check if the inode is in use and matches the current working directory
        if ((inode->used_size & 0x80) && // Inode is in use
            (inode->dir_parent & 0x7F) == pd && // Belongs to current directory
            strncmp(inode->name, name, 5) == 0) { // Name matches
            if (inode->dir_parent & 0x80) { // It's a directory
                cwd = i; // Change to the new directory
                return;
            } else {
                //printf("Debug 2: %i\n", cwd);
                fprintf(stderr, "Error: Directory %s does not exist\n", name);
                return;
            }
        }
    }

    // If no matching directory is found
    //printf("Debug 3: %i\n", cwd);
    fprintf(stderr, "Error: Directory %s does not exist\n", name);
    return;
}


/**
* MAIN for handling input commands
*/
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Command Error: , 0\n");
        return 1;
    }

    FILE *cmd_file = fopen(argv[1], "r");
    if (!cmd_file) {
        fprintf(stderr, "Command Error: %s, 0\n", argv[1]);
        return 1;
    }

    char line[2048];
    int line_num = 0;
    while (fgets(line, sizeof(line), cmd_file)) {
        line_num++;
        char cmd;
        char arg1[1025] = {0};
        int args_read = 0;

        args_read = sscanf(line, " %c", &cmd);

        if (args_read != 1) {
            fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
            continue;
        }

        // Handle each command and validate arguments count
        if (cmd == 'M') {
            // M <disk>
            if (sscanf(line, " %*c %1024s", arg1) != 1) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_mount(arg1);

        } else if (cmd == 'C') {
            // C <file> <size>
            int sz;
            if (sscanf(line, " %*c %1024s %d", arg1, &sz) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            if (strlen(arg1) > 5 || sz < 0 || sz > 127) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_create(arg1, sz);

        } else if (cmd == 'D') {
            // D <file>
            if (sscanf(line, " %*c %1024s", arg1) != 1 || strlen(arg1) > 5) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_delete(arg1);

        } else if (cmd == 'R') {
            // R <file> <block_num>
            int blk;
            if (sscanf(line, " %*c %1024s %d", arg1, &blk) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            if (strlen(arg1) > 5 || blk < 0 || blk > 127) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_read(arg1, blk);

        } else if (cmd == 'W') {
            // W <file> <block_num>
            int blk;
            if (sscanf(line, " %*c %1024s %d", arg1, &blk) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            if (strlen(arg1) > 5 || blk < 0 || blk > 127) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_write(arg1, blk);

        } else if (cmd == 'B') {
            // B <new buffer characters>
            // copy rest of line as buffer (trimming newline)
            char *p = strchr(line, ' ');
            if (!p) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            p++;
            size_t len = strlen(p);
            if (len > 0 && p[len-1] == '\n') p[len-1] = '\0';
            if (strlen(p) > 1024) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_buff(p);

        } else if (cmd == 'L') {
            // L
            char extra[10];
            if (sscanf(line, " %*c %9s", extra) == 1) {
                // extra args found
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_ls();

        } else if (cmd == 'E') {
            // E <file> <new_size>
            int new_sz;
            if (sscanf(line, " %*c %1024s %d", arg1, &new_sz) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            if (strlen(arg1) > 5 || new_sz < 1 || new_sz > 127) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_resize(arg1, new_sz);

        } else if (cmd == 'O') {
            // O
            char extra[10];
            if (sscanf(line, " %*c %9s", extra) == 1) {
                // extra args found
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }
            fs_defrag();

        } else if (cmd == 'Y') {
            // Y <directory name>
            if (sscanf(line, " %*c %1024s", arg1) != 1 || strlen(arg1) > 5) {
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }

            char extra[1025];
            int extra_matched = sscanf(line, " %*c %*s %1024s", extra);
            if (extra_matched == 1) {
                // Extra argument found
                fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
                continue;
            }

            fs_cd(arg1);

        } else {
            // Invalid command
            fprintf(stderr, "Command Error: %s, %d\n", argv[1], line_num);
        }
    }

    fclose(cmd_file);
    return 0;
}