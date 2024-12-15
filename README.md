# File System Simulator

## Overview

This project implements a simple file system simulator in C, allowing basic file and directory operations on a virtual disk. The simulator manages inodes, block allocation, and provides functionalities such as mounting a disk, creating and deleting files/directories, reading and writing data blocks, resizing files, defragmenting the file system, and navigating directories.

To use: `import "fs-sim.h"`

## Design Choices

### Inode Structure

Each inode represents a file or directory within the file system. The inode structure includes:

- **`name[5]`**: The name of the file or directory, limited to 5 characters.
- **`used_size`**: An 8-bit field where the highest bit indicates whether the inode is in use (`1`) or free (`0`). The lower 7 bits represent the size (number of blocks) for files or special values for directories.
- **`start_block`**: The starting block index of the file's data on the disk.
- **`dir_parent`**: The parent directory's inode index, with the highest bit indicating whether the inode represents a directory.

### Superblock

The superblock contains metadata about the file system, including:

- An array of inodes (`inode[126]`), supporting up to 126 files/directories.
- **`free_block_list`**: A bitmap representing the allocation status of 128 blocks on the virtual disk.

### Block Allocation

- The virtual disk is divided into 128 blocks, each 1KB in size.
- Block `0` is reserved for the superblock.
- Blocks `1-127` are available for file and directory data.
- Allocation ensures that files are stored in contiguous blocks to simplify defragmentation and resizing.

### Directory Structure

- The root directory is represented by inode index `0`.
- Directories store entries by referencing their parent inode.
- Special entries `.` and `..` represent the current and parent directories, respectively.

## System Calls Used

The following system calls from the C standard library are utilized to implement file system operations:

- **`open`**: To open the virtual disk file with read-write access.
- **`read`**: To read data from the disk, such as the superblock or file blocks.
- **`write`**: To write data to the disk, updating inodes and file blocks.
- **`lseek`**: To move the file pointer to specific block positions on the disk.
- **`close`**: To close file descriptors when unmounting disks or encountering errors.
- **`fopen`**: To open command files for processing.
- **`fgets`**: To read commands from the command file line by line.
- **`fprintf`**: To output error messages to `stderr`.
- **`printf`**: To output directory listings and other information to `stdout`.
- **`memset`**, **`memcpy`**, **`strncpy`**, **`strncmp`**: For memory and string manipulations.

## Implementation Details

### Mounting a Disk (`fs_mount`)

- **Functionality**: Opens the specified disk file and reads the superblock.
- **Consistency Checks**: 
  1. Verifies that free inodes have all bits zeroed.
  2. Ensures file inodes have valid `start_block` and `size`.
  3. Confirms directory inodes have zeroed `start_block` and appropriate `size`.
  4. Validates parent inode indices and their directory status.
  5. Checks for unique names within directories.
  6. Ensures block allocation consistency with the free block list.
- **System Calls**: `open`, `read`, `lseek`, `write`, `close`, `memcpy`, `strncmp`
- **Design Choice**: Utilizes comprehensive consistency checks to maintain filesystem integrity upon mounting.

### Creating a File or Directory (`fs_create`)

- **Functionality**: Creates a new file or directory in the current working directory.
- **Process**:
  1. Searches for a free inode.
  2. Checks for name uniqueness within the current directory.
  3. Allocates contiguous blocks if creating a file.
  4. Updates the free block list and initializes the inode.
- **System Calls**: `write`, `lseek`, `strncpy`, `strncmp`, `memset`
- **Design Choice**: Ensures efficient block allocation and prevents naming conflicts.

### Deleting a File or Directory (`fs_delete`)

- **Functionality**: Deletes a specified file or directory, recursively removing contents if it's a directory.
- **Process**:
  1. Locates the target inode by name within the current directory.
  2. If it's a directory, recursively deletes all its contents.
  3. Frees allocated blocks and resets the inode.
- **System Calls**: `write`, `lseek`, `memset`
- **Design Choice**: Implements recursive deletion to handle nested directories effectively.

### Reading and Writing Blocks (`fs_read`, `fs_write`)

- **`fs_read`**:
  - **Functionality**: Reads a specified block of a file into a buffer.
  - **Process**:
    1. Locates the file inode.
    2. Validates the block number.
    3. Reads the block data into the buffer.
  - **System Calls**: `lseek`, `read`
  
- **`fs_write`**:
  - **Functionality**: Writes data from a buffer to a specified block of a file.
  - **Process**:
    1. Locates the file inode.
    2. Validates the block number.
    3. Writes the buffer data to the specified block.
  - **System Calls**: `lseek`, `write`
  
- **Design Choice**: Separates read and write functionalities for modularity and clarity.

### Resizing a File (`fs_resize`)

- **Functionality**: Changes the size of a specified file by allocating or freeing blocks.
- **Process**:
  1. Locates the file inode.
  2. If increasing size, checks for contiguous free blocks or relocates the file.
  3. If decreasing size, frees the excess blocks.
  4. Updates the inode and free block list accordingly.
- **System Calls**: `lseek`, `read`, `write`, `memset`, `malloc`, `free`
- **Design Choice**: Maintains data integrity by ensuring blocks remain contiguous and handling relocation when necessary.

### Defragmenting the File System (`fs_defrag`)

- **Functionality**: Reorganizes files to occupy the lowest possible block indices, maintaining the order of files.
- **Process**:
  1. Sorts files based on their current start blocks.
  2. Moves each file to the next available contiguous blocks.
  3. Updates inodes and the free block list.
- **System Calls**: `lseek`, `read`, `write`, `memset`, `malloc`, `free`
- **Design Choice**: Enhances filesystem performance by minimizing fragmentation.

### Navigating Directories (`fs_cd`)

- **Functionality**: Changes the current working directory to a specified directory.
- **Process**:
  1. Handles special cases for `.` (current directory) and `..` (parent directory).
  2. Searches for the specified directory within the current directory.
  3. Updates the current directory index if found.
- **System Calls**: `strncmp`
- **Design Choice**: Simplifies directory navigation with clear handling of special directories.

### Listing Directory Contents (`fs_ls`)

- **Functionality**: Lists all files and directories in the current directory, including `.` and `..`.
- **Process**:
  1. Counts the number of files in the current and parent directories.
  2. Prints `.` and `..` with their respective counts.
  3. Iterates through inodes to list files and directories with their sizes or child counts.
- **System Calls**: `printf`, `memcpy`, `strncmp`
- **Design Choice**: Provides a clear and organized listing similar to Unix `ls` command.

### Buffer Management (`fs_buff`)

- **Functionality**: Manages a 1KB buffer for read and write operations.
- **Process**:
  1. Clears the existing buffer.
  2. Copies new data into the buffer.
- **System Calls**: `memset`, `memcpy`
- **Design Choice**: Ensures buffer integrity and readiness for subsequent operations.

### Command Handling (`main` Function)

- **Functionality**: Parses and executes commands from a command file.
- **Process**:
  1. Reads commands line by line from the input file.
  2. Parses each command and its arguments.
  3. Executes the corresponding file system function.
  4. Handles command errors and argument validation.
- **System Calls**: `fopen`, `fgets`, `sscanf`, `fprintf`, `fclose`
- **Design Choice**: Provides a flexible interface for batch processing of file system operations.

## Testing

The implementation was tested using a series of command files that simulate various file system operations. These were passed in through the provided input filles and compared using 'diff'. The binary files were examined using 'hexdump'.
The implementation was debugged by printing to stdout and stderr.

## Sources

- **Operating System Concepts** by Abraham Silberschatz, Peter B. Galvin, and Greg Gagne.
- **Linux Man Pages** for detailed explanations of system calls like `open`, `read`, `write`, and `lseek`.

