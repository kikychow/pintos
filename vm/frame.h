#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <hash.h>
#include "threads/palloc.h"
#include "vm/page.h"

/* Contains data in an entry within the frame table. */
struct frame_table_entry
  {
    void *kpage;                      /* Virtual address of frame. */
    void *upage;                      /* Virtual address of user page. */
    struct thread *own_thread;        /* Thread that owns user page. */

    struct hash_elem table_elem;      /* Hash element for frame table. */
    struct list_elem read_only_elem;  /* List element for read-only frame list. */
    struct list_elem all_elem;        /* List element for all frames list. */

    int number_of_pages_pointed;      /* # of pages using this frame. */
    struct list pages_pointed;        /* Contains all pages that point to frame. */

    struct file *file;                /* Process' executable file. */
    bool writable;                    /* Indicates if file can be written to. */
    off_t file_ofs;                   /* # of bytes offset from start of file. */

    bool pinned;                      /* Indicates if frame can be evicted. */
  };

void frame_table_init (void);
struct frame_table_entry *frame_to_evict (void);
struct frame_table_entry *frame_get_kpage (struct page *p, enum palloc_flags);
void frame_free_kpage (void *kpage, bool forced);

#endif /* vm/frame.h */
