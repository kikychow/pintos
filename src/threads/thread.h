#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <hash.h>
#include <stdint.h>
#include "threads/synch.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
typedef int pid_t;
typedef int fixed_point;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int base_priority;                  /* Thread base priority. */
    struct list_elem allelem;           /* List element for all threads list. */

    int nice;                           /* Thread nice value stored as an int. */
    fixed_point recent_cpu;             /* Thread recent_cpu value stored as a fixed point. */

    int64_t current_ticks;              /* Amount of current sleep ticks left.*/
    struct list_elem sleepingelem;      /* List element for sleeping list. */

    struct list child_pcb_list;         /* List of child processes' PCBs. */
    struct pcb *own_pcb;                /* Thread's process' PCB. */

    struct file *elf;                   /* Thread's ELF executable file. */
    struct list file_fd_table;          /* List of open files and their FD mappings. */
    struct list file_mmap_table;        /* List of files and their memory mappings. */

    /* Semaphore used to synchronise the loading of child processes. */
    struct semaphore wait_for_child_load_semaphore;

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */
    struct list held_locks_list;        /* List of locks thread is currently holding. */

    struct hash page_table;             /* Supplemental page table. */
    struct lock page_table_lock;        /* Lock for accessing page table. */
    void *esp;                          /* Current pointer of thread's stack frame. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* Process control block for a thread. */
struct pcb
  {
    int exit_status;                    /* Status when process exits. */

    /* Semaphore used to synchronise the exiting and waiting of child processes. */
    struct semaphore semaphore;

    struct list_elem childelem;         /* List element for parent thread's children list. */
    pid_t pid;                          /* Thread identifier of parent thread. */
    struct thread *parent;              /* Parent thread. */
    int load_status;                    /* Status when process finishes loading. */
    bool wait;                          /* Indicates whether process is waiting. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);
size_t threads_ready(void);

void free_pcb (struct pcb *p);
void free_all_child_pcb (struct thread *t);
struct pcb *find_pcb (tid_t tid);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_check_ticks (struct thread *t);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

void list_insert_desc_pri (struct list *, struct list_elem *, void *);
void list_sort_desc_pri (struct list *list);
void thread_check_if_yield (void);
int thread_get_priority (void);
int thread_get_priority_of (struct thread *t);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
