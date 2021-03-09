#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <list.h>

typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

/* A mapping between open files in a thread and their descriptors. */
struct file_fd_mapping
  {
    int fd;                             /* File descriptor. */
    struct file *file;                  /* File to be mapped to. */
    struct list_elem elem;              /* List element for open files. */
  };

/* A mapping between open files' FDs in a thread and their virtual addresses. */
struct file_mmap_mapping
  {
    mapid_t mapid;                      /* MMAP identifier. */
    struct file *file;                  /* File corresponding to FD. */
    void *addr;                         /* Virtual memory address. */
    struct list_elem elem;              /* List element for MMAP table. */
  };

void syscall_init (void);
void check_is_user_vaddr (const void *vaddr);
void check_pagedir_get_page (const void *uaddr);
void check_buffer_in_pagedir (const void *buffer, unsigned size);
void exit (int status);

bool free_file_fd_mapping (struct file_fd_mapping *mapping);
bool free_file_mmap_mapping (struct file_mmap_mapping *mapping);

#endif /* userprog/syscall.h */
