#ifndef FSSIM_H
#define FSSIM_H

#include <stdint.h>

typedef struct {
	char name[5];        // Name of the file/directory (not necessarily null terminated)
	uint8_t used_size;   // State of inode and size of the file/directory
	uint8_t start_block; // Index of the first block of the file/directory
	uint8_t dir_parent;  // Type of inode and index of the parent inode
} Inode;

typedef struct {
	char free_block_list[16];
	Inode inode[126];
} Superblock;

void fs_mount(char *new_disk_name);
void fs_create(char name[5], int size);
void fs_delete(char name[5]);
void fs_read(char name[5], int block_num);
void fs_write(char name[5], int block_num);
void fs_buff(char buff[1024]);
void fs_ls(void);
void fs_resize(char name[5], int new_size);
void fs_defrag(void);
void fs_cd(char name[5]);

# endif