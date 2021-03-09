#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "vm/frame.h"
#include "vm/swap.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"

struct page *
init_page (void *address, bool writable)
{
  struct page *p = malloc (sizeof (struct page));
  ASSERT (p != NULL);

  struct thread *t = thread_current ();

  p->virtual_address = pg_round_down (address);

  /* If thread's page table already contains page. */
  if (hash_insert (&t->page_table, &p->hash_elem) != NULL)
    {
      free (p);
      return NULL;
    }

  p->frame = NULL;
  p->own_thread = t;
  p->file = NULL;
  p->file_read_bytes = 0;
  p->file_ofs = 0;
  p->writable = writable;
  p->in_swap_slot = false;
  p->swap_index = 0;
  p->dirty = false;

  return p;
}

/* Creates a series of consecutive user pages from a given segment of a file. */
bool
load_pages_from_file (void *upage_addr, struct file *file, off_t ofs,
                      uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct thread *t = thread_current ();

  while ((int) read_bytes > 0 || (int) zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      lock_acquire (&t->page_table_lock);

      struct page *p = NULL;
      p = find_page (&t->page_table, upage_addr);

      if (p == NULL) p = init_page (upage_addr, writable);
      if (p == NULL) goto fail;

      p->writable = writable;
      p->file = file;
      p->file_read_bytes = page_read_bytes;
      p->file_ofs = ofs;

      lock_release (&t->page_table_lock);

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += PGSIZE;
      upage_addr += PGSIZE;
    }
  return true;

  fail:
  lock_release (&t->page_table_lock);
  return false;
}

void
free_page (struct hash_elem *e, void *aux UNUSED)
{
  struct page *p = hash_entry (e, struct page, hash_elem);

  if (p->frame != NULL) {
    struct frame_table_entry *fte;
    fte = p->frame;
    p->frame = NULL;

    /* Remove from frame pages pointed list. */
    if (!p->writable) list_remove (&p->list_elem);
    frame_free_kpage (fte->kpage, false);
  }

  p->frame = NULL;
  hash_delete (&thread_current ()->page_table, &p->hash_elem);
  pagedir_clear_page (thread_current ()->pagedir, p->virtual_address);
  free (p);
}

void
free_page_table (void)
{
  struct hash page_table = thread_current ()->page_table;
  hash_destroy (&page_table, free_page);
}

/* Returns whether page with given address exists in thread's page table. */
bool
can_locate_page (void *address)
{
  struct thread *t = thread_current ();

  if (address == NULL || &t->page_table == NULL)
    return false;

  lock_acquire (&t->page_table_lock);

  struct page *p = find_page (&t->page_table, pg_round_down (address));
  if (p == NULL) return false;

  if (p->frame == NULL) {
    if (!store_page_in_frame (p))
      {
        lock_release (&t->page_table_lock);
        return false;
      }
  }

  lock_release (&t->page_table_lock);

  if (pagedir_get_page (t->pagedir, p->virtual_address)) return true;

  bool success = pagedir_set_page (t->pagedir, p->virtual_address,
                                   p->frame->kpage, p->writable);
  return success;
}

/* Finds given page in a given hash table. Returns NULL if not found.
   Pre-condition: Page table's corresponding lock must be held before calling. */
struct page *
find_page (struct hash *page_table, void *address)
{
  if (address == NULL) return NULL;

  struct hash_elem *e;
  struct page p;

  p.virtual_address = pg_round_down (address);
  e = hash_find (page_table, &p.hash_elem);
  if (e == NULL) return NULL;

  return hash_entry (e, struct page, hash_elem);
}

/* Stores given page into a new kernel frame. */
bool
store_page_in_frame (struct page *p)
{
  ASSERT (lock_held_by_current_thread (&thread_current ()->page_table_lock));
  ASSERT (p != NULL);

  struct frame_table_entry *new_frame = frame_get_kpage (p, PAL_USER);
  if (new_frame == NULL) return false;

  p->frame = new_frame;

  if (p->in_swap_slot)
    {
      page_swap_in (p);
    }
  else if (p->file)
    {
      size_t page_read_bytes = p->file_read_bytes;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      off_t file_ofs = p->file_ofs;

      int result = file_read_at (p->file, new_frame->kpage, page_read_bytes, file_ofs);

      if (result != (int) page_read_bytes)
        {
          p->frame = NULL;
          frame_free_kpage (new_frame->kpage, false);
          return false;
        }
      memset (new_frame->kpage + page_read_bytes, 0, page_zero_bytes);
    }
  else
   {
     /* Provide all-zero page. */
     memset (p->frame->kpage, 0, PGSIZE);
   }

  return true;
}

/* Pre-condition: Page table's corresponding lock must be held before calling. */
void
page_swap_out (struct hash *page_table, void *upage)
{
  struct page *page_to_evict = find_page (page_table, upage);
  ASSERT (page_to_evict != NULL);
  page_to_evict->dirty = page_to_evict->dirty ||
                         pagedir_is_dirty (page_to_evict->own_thread->pagedir,
                                           upage);

  if (!page_to_evict->file)
    {
      /* Stack page. */
      page_to_evict->in_swap_slot = true;
      page_to_evict->swap_index = swap_out (page_to_evict->frame->kpage);
    }
  else if (page_to_evict->dirty)
    {
      /* Dirty page. */
      if (page_to_evict->writable &&
          page_to_evict->file == page_to_evict->own_thread->elf)
        {
          /* Writable executable file. */
          page_to_evict->in_swap_slot = true;
          page_to_evict->swap_index = swap_out (page_to_evict->frame->kpage);
        }
      else
        {
          file_write_at (page_to_evict->file, page_to_evict->frame->kpage,
                         page_to_evict->file_read_bytes, page_to_evict->file_ofs);
        }
    }
  page_to_evict->frame = NULL;
  pagedir_clear_page (page_to_evict->own_thread->pagedir,
                      page_to_evict->virtual_address);
}

/* Pre-condition: Page table's corresponding lock must be held before calling. */
void
page_swap_in (struct page *p)
{
  swap_in (p->frame->kpage, p->swap_index);
  p->in_swap_slot = false;
  p->swap_index = 0;
}

void
page_pin (void *upage)
{
  lock_acquire (&thread_current ()->page_table_lock);

  struct page *p = find_page (&thread_current ()->page_table, upage);
  p->frame->pinned = true;

  lock_release (&thread_current ()->page_table_lock);
}

void
page_unpin (void *upage)
{
  lock_acquire (&thread_current ()->page_table_lock);

  struct page *p = find_page (&thread_current ()->page_table, upage);
  p->frame->pinned = false;

  lock_release (&thread_current ()->page_table_lock);
}

unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  ASSERT (p_ != NULL);

  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->virtual_address, sizeof p->virtual_address);
}

bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED)
{
  ASSERT (a_ != NULL);
  ASSERT (b_ != NULL);

  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->virtual_address < b->virtual_address;
}
