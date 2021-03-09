#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "filesys/off_t.h"

/* User virtual page. */
struct page
  {
    struct frame_table_entry* frame;  /* Frame (if any) linked to user page. */
    void *virtual_address;            /* Virtual address for user page. */

    struct thread *own_thread;        /* Thread that owns this user page. */
    struct hash_elem hash_elem;       /* Hash element for thread's SPT. */
    struct list_elem list_elem;       /* List element of frame pages pointed list. */

    struct file *file;                /* Process' executable file. */
    size_t file_read_bytes;           /* # of bytes read starting from offset. */
    off_t file_ofs;                   /* # of bytes offset from start of file. */
    bool writable;                    /* Indicates if file can be written to. */
    bool dirty;                       /* Checks if page is dirty */

    bool in_swap_slot;                /* Indicates if page is stored in swap slot. */
    size_t swap_index;                /* Index indicating which swap slot the page is stored. */
  };

struct page *init_page (void *address, bool writable);
bool load_pages_from_file (void *upage_addr, struct file *file, off_t ofs,
                           uint32_t read_bytes, uint32_t zero_bytes,
                           bool writable);

void free_page (struct hash_elem *e, void *aux);
void free_page_table (void);

bool can_locate_page (void* address);
struct page *find_page (struct hash *page_table, void* address);
bool store_page_in_frame (struct page *p);

void page_swap_out (struct hash *page_table, void *upage);
void page_swap_in (struct page *p);

void page_pin (void * upage);
void page_unpin (void * upage);

hash_hash_func page_hash;
hash_less_func page_less;

#endif
