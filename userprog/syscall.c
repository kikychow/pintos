#include "userprog/syscall.h"
#include <stdio.h>
#include <stdbool.h>
#include <syscall-nr.h>
#include <list.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include <string.h>

static void syscall_handler (struct intr_frame *);
static void halt (void);
static pid_t exec (const char *cmd_line);
static int open (const char *file);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned size);
static int write (int fd, const void *buffer, unsigned size);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
static mapid_t mmap (int fd, void *addr);
static void munmap (mapid_t mapping);
static void pin_pages_for_buffer (const void *buffer, unsigned size);
static void unpin_pages_for_buffer (const void *buffer, unsigned size);

static void syscall_read_args (int args[], void *esp, int args_size);
static struct file_fd_mapping *find_file_fd_mapping (int fd);
static struct file_mmap_mapping *find_file_mmap_mapping (mapid_t mapid);

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

/* Gets arguments for system calls via the thread's stack's ESP. */
static void
syscall_read_args (int args[], void *esp, int args_size)
{
  int *ptr;
  for (int i = 0; i < args_size; i++)
    {
      ptr = (int *) esp + i + 1;
      args[i] = *ptr;
    }
}

static void
syscall_handler (struct intr_frame *f UNUSED)
{
  check_pagedir_get_page (f->esp);
  void *esp = f->esp;
  thread_current ()->esp = esp;
  int args[3];

  switch (* (int *) esp)
    {
      case SYS_HALT:
        halt ();
        break;

      case SYS_EXIT:
        syscall_read_args (args, esp, 1);
        exit (args[0]);
        break;

      case SYS_EXEC:
        syscall_read_args (args, esp, 1);
        f->eax = exec ((const char *) args[0]);
        break;

      case SYS_WAIT:
        syscall_read_args (args, esp, 1);
        f->eax = process_wait (args[0]);
        break;

      case SYS_CREATE:
        syscall_read_args (args, esp, 2);
        f->eax = create ((const char *) args[0], (unsigned) args[1]);
        break;

      case SYS_REMOVE:
        syscall_read_args (args, esp, 1);
        f->eax = remove ((const char *) args[0]);
        break;

      case SYS_OPEN:
        syscall_read_args (args, esp, 1);
        f->eax = open ((const char *) args[0]);
        break;

      case SYS_FILESIZE:
        syscall_read_args (args, esp, 1);
        f->eax = filesize (args[0]);
        break;

      case SYS_READ:
        syscall_read_args (args, esp, 3);
        f->eax = read (args[0], (void *) args[1], (unsigned) args[2]);
        break;

      case SYS_WRITE:
        syscall_read_args (args, esp, 3);
        f->eax = write (args[0], (const void *) args[1], (unsigned) args[2]);
        break;

      case SYS_SEEK:
        syscall_read_args (args, esp, 2);
        seek (args[0], (unsigned) args[1]);
        break;

      case SYS_TELL:
        syscall_read_args (args, esp, 1);
        f->eax = tell (args[0]);
        break;

      case SYS_CLOSE:
        syscall_read_args (args, esp, 1);
        close (args[0]);
        break;

      case SYS_MMAP:
        syscall_read_args (args, esp, 2);
        f->eax = mmap (args[0], (void *) args[1]);
        break;

      case SYS_MUNMAP:
        syscall_read_args (args, esp, 1);
        munmap (args[0]);
        break;

      default:
        exit (-1);
        break;
    }
}

/* Checks if a pointer is a valid address in the thread's page directory. */
void
check_pagedir_get_page (const void *uaddr)
{
  check_is_user_vaddr (uaddr);
  void *vaddr = pg_round_down(uaddr);
  if (pagedir_get_page (thread_current ()->pagedir, vaddr) == NULL)
    {
      if (!can_locate_page ((void *) vaddr)) {
        exit(-1);
      }
    }
}

/* Checks if a pointer is a valid user memory address. */
void
check_is_user_vaddr (const void *vaddr)
{
  if (!is_user_vaddr (vaddr))
    {
      exit (-1);
    }
}

/* Checks if a buffer is a valid address in the thread's page
   directory(s). */
void
check_buffer_in_pagedir (const void *buffer, unsigned size)
{
  const void *buffer_copy = buffer;
  for (unsigned i = 0; i < (size / PGSIZE) + 1; i++)
    {
      check_pagedir_get_page (buffer_copy);
      buffer_copy += PGSIZE;
    }
}

static void
halt (void)
{
  shutdown_power_off ();
}

void
exit (int status)
{
  struct thread *t = thread_current();

  if (status < 0)
    {
      status = -1;
    }

  t->own_pcb->exit_status = status;
  printf("%s: exit(%d)\n", t->name, status);
  thread_exit ();
}

static pid_t
exec (const char *cmd_line)
{
  check_pagedir_get_page (cmd_line);
  return process_execute (cmd_line);
}

static int
open (const char *file)
{
  /* The values 0 and 1 are reserved for standard input and output.
     The next usable value for FDs would be 2. */
  static int fd_counter = STDOUT_FILENO + 1;
  struct thread *t = thread_current ();

  check_pagedir_get_page (file);

  lock_acquire (&filesys_lock);

  struct file *file_opened = filesys_open (file);
  if (file_opened == NULL)
    {
      lock_release (&filesys_lock);
      return -1;
    }

  /* Every file-FD mapping will eventually be freed in process_exit(). */
  struct file_fd_mapping *fm =
          (struct file_fd_mapping *) malloc (sizeof (struct file_fd_mapping));
  if (fm == NULL)
    {
      file_close (file_opened);
      lock_release (&filesys_lock);
      return -1;
    }

  fm->fd = fd_counter;
  fm->file = file_opened;

  list_push_front (&t->file_fd_table, &fm->elem);

  fd_counter++;

  lock_release (&filesys_lock);
  return fm->fd;
}

static bool
create (const char *file, unsigned initial_size)
{
  bool file_created;

  check_pagedir_get_page (file);

  lock_acquire (&filesys_lock);
  file_created = filesys_create (file, initial_size);
  lock_release (&filesys_lock);

  return file_created;
}

static bool
remove (const char *file)
{
  bool file_removed;

  check_pagedir_get_page (file);

  lock_acquire (&filesys_lock);
  file_removed = filesys_remove (file);
  lock_release (&filesys_lock);

  return file_removed;
}

static int
filesize (int fd)
{
  int file_size;

  lock_acquire (&filesys_lock);

  struct file_fd_mapping *file_fd_mapping = find_file_fd_mapping (fd);
  if (file_fd_mapping == NULL || file_fd_mapping->file == NULL)
    {
      lock_release (&filesys_lock);
      return -1;
    }
  struct file *file = file_fd_mapping->file;

  file_size = (int) file_length (file);

  lock_release (&filesys_lock);
  return file_size;
}

static void
pin_pages_for_buffer (const void *buffer, unsigned size)
{
  void *upage;
  for(upage = pg_round_down (buffer); upage < buffer + size; upage += PGSIZE)
    {
      if (can_locate_page (upage)) page_pin (upage);
    }
}

static void
unpin_pages_for_buffer (const void *buffer, unsigned size)
{
  void *upage;
  for(upage = pg_round_down (buffer); upage < buffer + size; upage += PGSIZE)
    {
      page_unpin (upage);
    }
}

static int
read (int fd, void *buffer, unsigned size)
{
  check_buffer_in_pagedir (buffer, size);

  if (fd == (int) STDOUT_FILENO) return -1;
  if (fd == (int) STDIN_FILENO)
    {
      for (unsigned i = 0; i < size; i++)
        {
          memset (buffer, input_getc (), 1);
          buffer++;
        }
      return size;
    }

  lock_acquire (&filesys_lock);

  struct file_fd_mapping *file_fd_mapping = find_file_fd_mapping (fd);
  if (file_fd_mapping == NULL || file_fd_mapping->file == NULL)
    {
      lock_release (&filesys_lock);
      return -1;
    }
  struct file *file = file_fd_mapping->file;

  pin_pages_for_buffer (buffer, size);
  off_t offset = file_read (file, buffer, size);
  unpin_pages_for_buffer (buffer, size);

  lock_release (&filesys_lock);
  return offset;
}

static int
write (int fd, const void *buffer, unsigned size)
{
  check_buffer_in_pagedir (buffer, size);

  if (fd == (int) STDIN_FILENO) return -1;
  if (fd == (int) STDOUT_FILENO)
    {
      putbuf (buffer, size);
      return size;
    }

  lock_acquire (&filesys_lock);

  struct file_fd_mapping *file_fd_mapping = find_file_fd_mapping (fd);
  if (file_fd_mapping == NULL || file_fd_mapping->file == NULL)
    {
      lock_release (&filesys_lock);
      return -1;
    }
  struct file *file = file_fd_mapping->file;

  pin_pages_for_buffer (buffer, size);
  off_t offset = file_write (file, buffer, size);
  unpin_pages_for_buffer (buffer, size);

  lock_release (&filesys_lock);
  return offset;
}

static void
seek (int fd, unsigned position)
{
  lock_acquire (&filesys_lock);

  struct file_fd_mapping *file_fd_mapping = find_file_fd_mapping (fd);
  if (file_fd_mapping == NULL)
    {
      lock_release (&filesys_lock);
      return;
    }


  struct file *file = file_fd_mapping->file;
  if (file == NULL)
    {
      lock_release (&filesys_lock);
      return;
    }

  file_seek (file, position);

  lock_release (&filesys_lock);
}

static unsigned
tell (int fd)
{
  unsigned file_curr_pos;

  lock_acquire (&filesys_lock);

  struct file_fd_mapping *file_fd_mapping = find_file_fd_mapping (fd);
  if (file_fd_mapping == NULL || file_fd_mapping->file == NULL)
    {
      lock_release (&filesys_lock);
      return -1;
    }
  struct file *file = file_fd_mapping->file;

  file_curr_pos = (unsigned) file_tell (file);

  lock_release (&filesys_lock);
  return file_curr_pos;
}

static void
close (int fd)
{
  struct file_fd_mapping *fd_mapping = find_file_fd_mapping (fd);
  if (fd_mapping == NULL || !free_file_fd_mapping (fd_mapping)) exit (-1);
}

static mapid_t
mmap (int fd, void *addr)
{
  static mapid_t mapid_counter = 2;
  struct thread *t = thread_current ();

  if (fd == (int) STDIN_FILENO || fd == (int) STDOUT_FILENO || addr == 0 ||
      (int) addr % PGSIZE != 0)
    return MAP_FAILED;

  struct file_fd_mapping *file_fd_mapping = find_file_fd_mapping (fd);
  if (file_fd_mapping == NULL) exit (-1);
  struct file *file = file_fd_mapping->file;
  if (file == NULL) exit (-1);
  if (file_length (file) == 0) return MAP_FAILED;

  /* Creates a copy of the file by reopening it. */
  struct file *file_copy = file_reopen (file);
  ASSERT (file_copy != NULL);

  /* Verifies if the pages to be allocated are not in use. */
  for (int i = 0; i < file_length (file_copy); i += PGSIZE)
    {
      if (find_page (&thread_current ()->page_table , addr + i)) return MAP_FAILED;
    }

  /* Creates user pages for the file in a segment loading fashion. */
  if (!load_pages_from_file (addr, file_copy, 0, file_length (file_copy),
                             0, true))
    return MAP_FAILED;

  struct file_mmap_mapping *fm =
          (struct file_mmap_mapping *) malloc (sizeof (struct file_mmap_mapping));
  if (fm == NULL) exit (-1);
  fm->mapid = mapid_counter;
  fm->file = file_copy;
  fm->addr = addr;

  list_push_front (&t->file_mmap_table, &fm->elem);

  mapid_counter++;

  return fm->mapid;
}

static void
munmap (mapid_t mapping)
{
  struct file_mmap_mapping *mmap_mapping = find_file_mmap_mapping (mapping);
  if (mmap_mapping == NULL || !free_file_mmap_mapping (mmap_mapping)) exit (-1);
}

/* Gets an open file in the current thread by its file descriptor by iterating
   through its list of open files at O(n) time complexity. */
static struct file_fd_mapping *
find_file_fd_mapping (int fd)
{
  struct thread *t = thread_current ();

  if (!list_empty (&t->file_fd_table))
    {
      for (struct list_elem *e = list_begin (&t->file_fd_table);
           e != list_end (&t->file_fd_table);
           e = list_next (e))
        {
          struct file_fd_mapping *fm = list_entry (e, struct file_fd_mapping, elem);
          if (fm->fd == fd) return fm;
        }
    }

  /* Returns NULL if no such open file exists in the list. */
  return NULL;
}

/* Gets an MMAP mapping by its MMAP ID by iterating through its MMAP table at
   O(n) time complexity. */
static struct file_mmap_mapping *
find_file_mmap_mapping (mapid_t mapid)
{
  struct thread *t = thread_current ();

  if (!list_empty (&t->file_mmap_table))
    {
      for (struct list_elem *e = list_begin (&t->file_mmap_table);
           e != list_end (&t->file_mmap_table);
           e = list_next (e))
        {
          struct file_mmap_mapping *fm = list_entry (e, struct file_mmap_mapping, elem);
          if (fm->mapid == mapid) return fm;
        }
    }

  /* Returns NULL if no such open file exists in the table. */
  return NULL;
}

bool free_file_fd_mapping (struct file_fd_mapping *mapping)
{
  ASSERT (mapping != NULL);

  list_remove (&mapping->elem);

  struct file *file = mapping->file;
  free (mapping);
  if (file == NULL) return false;

  lock_acquire (&filesys_lock);
  /* Initially opened and malloc'd in open() in syscall.c. */
  file_close (file);
  lock_release (&filesys_lock);

  return true;
}

bool free_file_mmap_mapping (struct file_mmap_mapping *mapping)
{
  ASSERT (mapping != NULL);

  struct file *file_copy = mapping->file;

  for (int i = 0; i < file_length (file_copy); i += PGSIZE)
    {
      void *upage_vaddr = mapping->addr + i;
      struct page *upage = find_page (&thread_current ()->page_table, upage_vaddr);
      if (upage == NULL) return false;

      if (pagedir_is_dirty (upage->own_thread->pagedir, upage_vaddr))
        {
          file_write_at (file_copy, upage_vaddr, upage->file_read_bytes, upage->file_ofs);
        }
      free_page (&upage->hash_elem, NULL);
    }

  list_remove (&mapping->elem);
  free (mapping);

  return true;
}
