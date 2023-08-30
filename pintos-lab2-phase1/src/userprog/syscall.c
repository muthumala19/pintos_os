#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <user/syscall.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

#define MAXIMUM_ARGS_COUNT 3
#define STD_INPUT 0
#define STD_OUTPUT 1

// decleration
int get_page(const void *vaddr);
void children_remove(void);
struct file *get_file(int filedes);
struct child_process *find_child(int pid);

int add_file(struct file *file_name);
bool create(const char *file_name, unsigned starting_size);
bool remove(const char *file_name);
int open(const char *file_name);
int read(int filedes, void *buffer, unsigned length);
int write(int filedes, const void *buffer, unsigned byte_size);
void seek(int filedes, unsigned new_position);
void pointer_validator(const void *vaddr);
void buffer_validator(const void *buf, unsigned byte_size);
void string_validator(const void *str);

void close_file(int file_descriptor);
void exit(int status);
static void syscall_handler(struct intr_frame *);
void stack_access(struct intr_frame *f, int *arg, int num_of_args);
bool IS_FILE_LOCKED = false;

/*
 * System call initializer
 * It handles the set up for system call operations.
 */
void syscall_init(void){
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame *f UNUSED)
{
  // Check if file lock has been initialized
  if (!IS_FILE_LOCKED)
  {
    lock_init(&fs_lock);
    IS_FILE_LOCKED = true;
  }

  int arg[MAXIMUM_ARGS_COUNT];
  int esp = get_page((const void *)f->esp);

  // Get system call number
  int syscall_num = *(int *)esp;

  switch (syscall_num)
  {

  /* Read from a file. */
  case SYS_READ:
    stack_access(f, &arg[0], 3);
    buffer_validator((const void *)arg[1], (unsigned)arg[2]);
    arg[1] = get_page((const void *)arg[1]);
    f->eax = read(arg[0], (void *)arg[1], (unsigned)arg[2]);
    break;

  /* Write to a file. */
  case SYS_WRITE:
    stack_access(f, &arg[0], 3);
    buffer_validator((const void *)arg[1], (unsigned)arg[2]);
    arg[1] = get_page((const void *)arg[1]);
    f->eax = write(arg[0], (const void *)arg[1], (unsigned)arg[2]);
    break;


  /* Wait for child process to die. */
//  case SYS_WAIT:
//    // Retrieve child process ID from stack
//    stack_access(f, &arg[0], 1);
//    // Wait for child process to finish
//    f->eax = process_wait(arg[0]);
//    break;

  /* Create a file. */
  case SYS_CREATE:
    // Retrieve file name and size from stack
    stack_access(f, &arg[0], 2);
    // Validate file name
    string_validator((const void *)arg[0]);
    // Get page pointer
    arg[0] = get_page((const void *)arg[0]);
    // Create file
    f->eax = create((const char *)arg[0], (unsigned)arg[1]);
    break;

  /* Delete a file. */
  case SYS_REMOVE:
    // Retrieve file name from stack
    stack_access(f, &arg[0], 1);
    // Validate file name
    string_validator((const void *)arg[0]);
    // Get page pointer
    arg[0] = get_page((const void *)arg[0]);
    // Remove file
    f->eax = remove((const char *)arg[0]);
    break;

  /* Change position in a file. */
  case SYS_SEEK:
    stack_access(f, &arg[0], 2);
    seek(arg[0], (unsigned)arg[1]);
    break;


  /* Open a file. */
  case SYS_OPEN:
    // Retrieve file name from stack
    stack_access(f, &arg[0], 1);
    // Validate file name
    string_validator((const void *)arg[0]);
    // Get page pointer
    arg[0] = get_page((const void *)arg[0]);
    // Open file
    f->eax = open((const char *)arg[0]);
    break;

  /* Close a file. */
  case SYS_CLOSE:
    stack_access(f, &arg[0], 1);
    lock_acquire(&fs_lock);
    close_file(arg[0]);
    lock_release(&fs_lock);
    break;

  /* Terminate this process. */
  case SYS_EXIT:
    // Retrieve exit status from stack
    stack_access(f, &arg[0], 1);
    // Exit process with given status
    exit(arg[0]);
    break;

  default:
    break;
  }
}

void stack_access(struct intr_frame *f, int *args, int num_of_args)
{
  // Pointer to traverse the stack
  int *ptr;

  // Loop through the number of arguments
  for (int i = 0; i < num_of_args; i++)
  {
    // Set the pointer to the current argument location on the stack
    ptr = (int *)f->esp + i + 1;
    // Validate the pointer to ensure it's within the user space
    pointer_validator((const void *)ptr);
    // Retrieve the argument value
    args[i] = *ptr;
  }
}

/* Exits the current thread with the given status */
void exit(int status)
{
  // Get the current thread
  struct thread *curr_thread = thread_current();

  // If the current thread has a parent and it's a child process
  if (check_thread_active(curr_thread->parent) && curr_thread->child_process)
  {
    // If the status is negative, set it to -1
    if (status < 0)
      status = -1;

    // Set the status of the child process
    curr_thread->child_process->status = status;
  }

  // Print the exit message
  printf("%s: exit(%d)\n", curr_thread->name, status);

  // Exit the thread
  thread_exit();
}

/* Creates a file with the given name and initial size */
bool create(const char *file_name, unsigned initial_size)
{
  // Acquire the file system lock
  lock_acquire(&fs_lock);

  // Create the file
  bool success = filesys_create(file_name, initial_size);

  // Release the file system lock
  lock_release(&fs_lock);

  // Return the result
  return success;
}

/* Removes a file with the given file name
   Returns true if successful, false otherwise */
bool remove(const char *file_name)
{
  // Acquire the file system lock before accessing it
  lock_acquire(&fs_lock);

  // Call filesys_remove to remove the file
  bool success = filesys_remove(file_name);

  // Release the file system lock
  lock_release(&fs_lock);

  // Return the success status of removing the file
  return success;
}

/* Opens a file with the given file name
   Returns the file descriptor if successful, SYS_ERROR otherwise */
int open(const char *file_name)
{
  // Acquire the file system lock before accessing it
  lock_acquire(&fs_lock);

  // Call filesys_open to open the file
  struct file *file_ptr = filesys_open(file_name);

  // If the file pointer is null, release the lock and return SYS_ERROR
  if (!file_ptr)
  {
    lock_release(&fs_lock);
    return SYS_ERROR;
  }

  // Call add_file to add the file to the current thread's file list
  int file_des = add_file(file_ptr);

  // Release the file system lock
  lock_release(&fs_lock);

  // Return the file descriptor
  return file_des;
}

/* Adds a file to the current thread's file list
   Returns the file descriptor if successful, SYS_ERROR otherwise */
int add_file(struct file *file_ptr)
{
  // Allocate memory for the process_file structure
  struct process_file *file_struct = malloc(sizeof(struct process_file));

  // If memory allocation fails, return SYS_ERROR
  if (!file_struct)
    return SYS_ERROR;

  // Fill in the fields of the process_file structure
  file_struct->file = file_ptr;
  file_struct->fd = thread_current()->fd;
  thread_current()->fd++;
  list_push_back(&thread_current()->file_name_list, &file_struct->elem);

  // Return the file descriptor
  return file_struct->fd;
}

// read function implementation
int read(int filedes, void *buffer, unsigned length)
{
  // Return 0 if length is 0 or negative
  if (length <= 0)
    return 0;

  // Read from standard input if filedes is STD_INPUT
  if (filedes == STD_INPUT)
  {
    uint8_t *buf = (uint8_t *)buffer;
    for (unsigned i = 0; i < length; i++)
      buf[i] = input_getc();

    return length;
  }

  // Acquire lock to access file system
  lock_acquire(&fs_lock);

  // Get file pointer using filedes
  struct file *file_ptr = get_file(filedes);
  if (!file_ptr)
  {
    lock_release(&fs_lock);
    return SYS_ERROR;
  }

  // Read data from the file
  int size = file_read(file_ptr, buffer, length);

  // Release lock and return the number of bytes read
  lock_release(&fs_lock);
  return size;
}

// write function implementation
int write(int filedes, const void *buffer, unsigned byte_size)
{
  // Return 0 if byte_size is 0 or negative
  if (byte_size <= 0)
    return 0;

  // Write to standard output if filedes is STD_OUTPUT
  if (filedes == STD_OUTPUT)
  {
    putbuf(buffer, byte_size);
    return byte_size;
  }

  // Acquire lock to access file system
  lock_acquire(&fs_lock);

  // Get file pointer using filedes
  struct file *file_ptr = get_file(filedes);
  if (!file_ptr)
  {
    lock_release(&fs_lock);
    return SYS_ERROR;
  }

  // Write data to the file
  int size = file_write(file_ptr, buffer, byte_size);

  // Release lock and return the number of bytes written
  lock_release(&fs_lock);
  return size;
}

/* Returns the file associated with the given file descriptor */
struct file *
get_file(int filedes)
{
  // Get the current thread
  struct thread *t = thread_current();
  // Initialize variables to traverse the list of process files
  struct list_elem *next;
  struct list_elem *e = list_begin(&t->file_name_list);

  // Iterate over the list of process files
  for (; e != list_end(&t->file_name_list); e = next)
  {
    next = list_next(e);
    // Get the current process file
    struct process_file *ptr_processing_file = list_entry(e, struct process_file, elem);

    // Check if the file descriptor matches the given one
    if (filedes == ptr_processing_file->fd)
      return ptr_processing_file->file;
  }

  // Return NULL if file not found
  return NULL;
}

/* function to change the position of the file pointer */
void seek(int filedes, unsigned new_position)
{
  // Acquire lock to access file system
  lock_acquire(&fs_lock);

  // Get the file associated with the file descriptor
  struct file *file_ptr = get_file(filedes);

  // If the file does not exist, release lock and return error
  if (!file_ptr)
  {
    lock_release(&fs_lock);
    return;
  }

  // Change the position of the file pointer
  file_seek(file_ptr, new_position);

  // Release lock after accessing the file
  lock_release(&fs_lock);
}

/* This function validates the virtual address provided as an argument. If the address

is not within the user virtual address space or is not a user virtual address,
the function exits with a system error status.
*/
void pointer_validator(const void *vaddr)
{
  if (vaddr < USER_VADDR_BOTTOM || !is_user_vaddr(vaddr))
  {
    exit(SYS_ERROR);
  }
}
/* This function validates the string located at the virtual address provided as an argument.

The function uses the get_page function to ensure that the pages containing each character of
the string are accessible. If any page is not accessible, the function exits with a system
error status.
*/
void string_validator(const void *str)
{
  for (; *(char *)get_page(str) != 0; str = (char *)str + 1)
  {
    /*Empty body */
  }
}

/* This function validates a buffer located at the virtual address provided as an argument.

The function uses the pointer_validator function to ensure that each byte in the buffer
is accessible.
*/
void buffer_validator(const void *buf, unsigned byte_size)
{
  unsigned i = 0;
  char *local_buffer = (char *)buf;
  for (; i < byte_size; i++)
  {
    pointer_validator((const void *)local_buffer);
    local_buffer++;
  }
}
/* This function retrieves the page containing the virtual address provided as an argument

using the pagedir_get_page function. If the page is not accessible, the function exits
with a system error status.
*/
int get_page(const void *vaddr)
{
  void *ptr = pagedir_get_page(thread_current()->pagedir, vaddr);
  if (!ptr)
  {
    exit(SYS_ERROR);
  }
  return (int)ptr;
}

/* Find a child process with given PID */
struct child_process *find_child(int pid)
{
  /* Get the current thread */
  struct thread *t = thread_current();
  /* List element pointers */
  struct list_elem *e, *next;

  /* Iterate through the child process list */
  for (e = list_begin(&t->child_process_list); e != list_end(&t->child_process_list); e = next)
  {
    next = list_next(e);
    /* Get the child process data */
    struct child_process *child_process = list_entry(e, struct child_process, elem);

    /* Check if the PID matches */
    if (pid == child_process->pid)
    {
      return child_process;
    }
  }

  /* If not found, return NULL */
  return NULL;
}

/* Remove all child processes */
void children_remove(void)
{
  /* Get the current thread */
  struct thread *t = thread_current();
  /* List element pointers */
  struct list_elem *e, *next;

  /* Iterate through the child process list */
  for (e = list_begin(&t->child_process_list); e != list_end(&t->child_process_list); e = next)
  {
    next = list_next(e);
    /* Get the child process data */
    struct child_process *child_process = list_entry(e, struct child_process, elem);

    /* Remove the child process from the list */
    list_remove(&child_process->elem);
    /* Free the child process data */
    free(child_process);
  }
}

// Function to close a file given a file descriptor
void close_file(int file_descriptor)
{
  // Get the current thread
  struct thread *t = thread_current();

  // Traverse the list of open files for the current thread
  struct list_elem *next;
  struct list_elem *e = list_begin(&t->file_name_list);

  for (; e != list_end(&t->file_name_list); e = next)
  {
    // Store the next element in the list
    next = list_next(e);

    // Get the current process file
    struct process_file *ptr_processing_file = list_entry(e, struct process_file, elem);

    // If the file descriptor matches the one being closed
    if (file_descriptor == ptr_processing_file->fd || file_descriptor == ALL_FDESC_CLOSE)
    {
      // Close the file
      file_close(ptr_processing_file->file);

      // Remove the process file from the list
      list_remove(&ptr_processing_file->elem);

      // Free the memory for the process file
      free(ptr_processing_file);

      // If not closing all file descriptors, return
      if (file_descriptor != ALL_FDESC_CLOSE)
        return;
    }

  }
}
