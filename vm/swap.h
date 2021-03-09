#ifndef VM_SWAP_H
#define VM_SWAP_H
#include <stddef.h>
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE ( PGSIZE / BLOCK_SECTOR_SIZE )

void swap_init (void);
size_t swap_out (void *kpage);
void swap_in (void *kpage, size_t swap_index);

#endif
