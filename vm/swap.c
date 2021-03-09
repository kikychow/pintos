#include "vm/swap.h"
#include "devices/block.h"
#include <bitmap.h>
#include "userprog/syscall.h"

/* Disk partition used for page swapping. */
static struct block *swap_space;
/* Swap table indicating which pages are within the swap space. */
static struct bitmap *swap_table;
/* Size of swap table. */
static size_t swap_table_size;

void
swap_init (void)
{
  swap_space = block_get_role (BLOCK_SWAP);
  swap_table_size = block_size (swap_space) / SECTORS_PER_PAGE;
  swap_table = bitmap_create (swap_table_size);
}

/* Swap out a page to a free swap slot. */
size_t
swap_out (void *kpage)
{
  size_t swap_index = bitmap_scan_and_flip (swap_table, 0, 1, false);
  if (swap_index == BITMAP_ERROR) exit (-1);

  for (int i = 0; i < SECTORS_PER_PAGE; i++)
    {
      block_write (swap_space, swap_index * SECTORS_PER_PAGE + i,
                   kpage + BLOCK_SECTOR_SIZE * i);
    }
  return swap_index;
}

/* Swap in the page and free the swap slot. */
void
swap_in (void *kpage, size_t swap_index)
{
  for (int i = 0; i < SECTORS_PER_PAGE; i++)
    {
      block_read (swap_space, swap_index * SECTORS_PER_PAGE + i,
                  kpage + BLOCK_SECTOR_SIZE * i);
    }
  bitmap_set (swap_table, swap_index, false);
}
