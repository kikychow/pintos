#include "vm/frame.h"
#include <stdbool.h>
#include <hash.h>
#include <debug.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "threads/thread.h"
#include "vm/swap.h"
#include "vm/page.h"

/* Frame table containing all frames. */
static struct hash frame_table;
/* Lock for accessing frame table. */
static struct lock frame_table_lock;

/* List of read-only frames. */
static struct list read_only_frame_list;
/* List of all frames (for circular queueing). */
static struct list all_frame_list;

static unsigned frame_hash (const struct hash_elem *e, void *aux);
static bool frame_less (const struct hash_elem *a, const struct hash_elem *b,
                        void *aux);

/* Initialises the frame table system. */
void
frame_table_init (void)
{
  lock_init (&frame_table_lock);
  hash_init (&frame_table, &frame_hash, &frame_less, NULL);
  list_init (&read_only_frame_list);
  list_init (&all_frame_list);
}

struct frame_table_entry *
frame_to_evict (void)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  struct list_elem *curr_elem = list_begin (&all_frame_list);
  struct frame_table_entry *curr_fte =
          list_entry (curr_elem, struct frame_table_entry, all_elem);

  /* Second chance page replacement algorithm. */
  while (true)
    {
      if (!curr_fte->pinned)
        {
          if (pagedir_is_accessed (curr_fte->own_thread->pagedir, curr_fte->upage))
            {
              pagedir_set_accessed (curr_fte->own_thread->pagedir,
                                    curr_fte->upage, false);
            }
          break;
        }

      /* Circular queue. */
      curr_elem = curr_elem == list_rbegin (&all_frame_list) ?
                  list_begin (&all_frame_list) : list_next (curr_elem);
      curr_fte = list_entry (curr_elem, struct frame_table_entry, all_elem);
    }
  return curr_fte;
}

/* Allocates a user page to a frame. */
struct frame_table_entry *
frame_get_kpage (struct page *p, enum palloc_flags flags UNUSED)
{
  struct thread *t = thread_current ();
  void *upage = p->virtual_address;
  struct frame_table_entry *fte = NULL;

  if (!p->writable)
    {
      lock_acquire (&frame_table_lock);
      for (struct list_elem *e = list_begin (&read_only_frame_list);
           e != list_end (&read_only_frame_list);
           e = list_next (e))
        {
          struct frame_table_entry *fte_temp =
                  list_entry (e, struct frame_table_entry, read_only_elem);
          if (p->file == fte_temp->file && p->file_ofs == fte_temp->file_ofs)
            {
              fte = fte_temp;
              fte->number_of_pages_pointed += 1;
              list_push_back (&fte->pages_pointed, &p->list_elem);
              goto done;
            }
        }
      lock_release (&frame_table_lock);
    }

  fte = (struct frame_table_entry *) malloc (sizeof (struct frame_table_entry));
  fte->file = p->file;
  fte->file_ofs = p->file_ofs;
  fte->number_of_pages_pointed = 1;
  fte->writable = p->writable;

  lock_acquire (&frame_table_lock);

  if (!p->writable)
    {
      list_init (&fte->pages_pointed);
      list_push_back (&fte->pages_pointed, &p->list_elem);
    }

  void *kpage = palloc_get_page (flags);
  if (kpage == NULL)
    {
      /* Swap out a page. */
      struct frame_table_entry *fte_to_evict = frame_to_evict ();

      if (t != fte_to_evict->own_thread)
        lock_acquire (&fte_to_evict->own_thread->page_table_lock);
      page_swap_out (&fte_to_evict->own_thread->page_table, fte_to_evict->upage);
      if (t != fte_to_evict->own_thread)
        lock_release (&fte_to_evict->own_thread->page_table_lock);

      frame_free_kpage (fte_to_evict->kpage, true);
      kpage = palloc_get_page (flags);
      ASSERT (kpage != NULL);
    }

  lock_release (&frame_table_lock);

  fte->kpage = kpage;
  fte->upage = upage;
  fte->own_thread = thread_current ();
  fte->pinned = false;

  lock_acquire (&frame_table_lock);
  if (!fte->writable) list_push_back (&read_only_frame_list, &fte->read_only_elem);
  hash_insert (&frame_table, &fte->table_elem);
  list_push_back (&all_frame_list, &fte->all_elem);

  done:
  lock_release (&frame_table_lock);
  return fte;
}

/* Frees a specific frame and deallocates the associated user page.
   Frees all pages sharing this frame if the 'forced' flag is true. */
void
frame_free_kpage (void *kpage, bool forced)
{
  ASSERT (kpage != NULL);
  struct hash_elem *e;
  struct frame_table_entry f;
  bool locked = false;

  if (!lock_held_by_current_thread (&frame_table_lock))
    {
      lock_acquire (&frame_table_lock);
      locked = true;
    }

  f.kpage = kpage;
  e = hash_find (&frame_table, &f.table_elem);
  if (e == NULL) goto done;

  struct frame_table_entry *fte =
          hash_entry (e, struct frame_table_entry, table_elem);
  if (fte == NULL) goto done;

  if (!fte->writable)
    {
      if (forced)
        {
          for (struct list_elem *le = list_begin (&fte->pages_pointed);
               le != list_end (&fte->pages_pointed);
               le = list_next (le))
            {
              struct page *p = list_entry (le, struct page, list_elem);
              p->frame = NULL;
              list_remove (&p->list_elem);
              fte->number_of_pages_pointed -= 1;
            }
        }
      else
        {
          fte->number_of_pages_pointed -= 1;
        }

      if (fte->number_of_pages_pointed == 0)
        {
          list_remove (&fte->read_only_elem);
        }
    }

  hash_delete (&frame_table, &fte->table_elem);
  list_remove (&fte->all_elem);
  palloc_free_page (kpage);
  free (fte);

  done:
  if (locked) lock_release (&frame_table_lock);
}

static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  const struct frame_table_entry *fte =
          hash_entry (e, struct frame_table_entry, table_elem);

  ASSERT (fte != NULL);
  return hash_bytes (&fte->kpage, sizeof (fte->kpage));
}

static bool
frame_less (const struct hash_elem *a, const struct hash_elem *b,
            void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);
  const struct frame_table_entry *fte_a =
          hash_entry (a, struct frame_table_entry, table_elem);
  const struct frame_table_entry *fte_b =
          hash_entry (b, struct frame_table_entry, table_elem);

  ASSERT (fte_a != NULL);
  ASSERT (fte_b != NULL);
  return fte_a->kpage < fte_b->kpage;
}
