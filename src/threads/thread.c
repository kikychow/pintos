#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/vaddr.h"
#include "threads/fixed-point.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-mlfqs". */
bool thread_mlfqs;
fixed_point load_avg;

static int changed = 0;
static struct thread *changed_threads[4];

static void kernel_thread (thread_func *, void *aux);

static bool desc_pri_func (const struct list_elem *a,
                           const struct list_elem *b, void *aux UNUSED);
static void thread_set_load_avg (void);
static void thread_calculate_recent_cpu_of (struct thread *t, void *aux);
static int thread_get_priority_mlfqs (struct thread *t);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static int clamp(int given, int high, int low);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  load_avg = 0;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Finds a given thread's PCB at O(n) time complexity. */
struct
pcb *find_pcb (tid_t tid)
{
  struct list_elem *e;
  struct thread *curr_t = thread_current ();
  for (e = list_begin (&curr_t->child_pcb_list); e != list_end (&curr_t->child_pcb_list);
       e = list_next (e))
    {
      struct pcb *p = list_entry (e, struct pcb, childelem);
      if (p->pid == tid)
        {
          return p;
        }
    }
  return NULL;
}

/* Whenever we call free_pcb, we always call list_remove. */
void free_pcb (struct pcb *p)
{
  list_remove (&p->childelem);
  if (p) free (p);
}

/* Frees every PCB on a thread's children processes list. */
void free_all_child_pcb (struct thread *t)
{
  struct list_elem *e;
  for (e = list_begin (&t->child_pcb_list); e != list_end (&t->child_pcb_list);
       e = list_next (e))
    {
      struct pcb *p = list_entry (e, struct pcb, childelem);

      /* A positive semaphore value indicates that a PCB is no longer being
         used. See process_exit() in process.c for implementation. */
      if (sema_try_down (&p->semaphore))
        {

          /* When we free a pcb, the struct elem e will also get freed, so when
             we run list_next on e, it will fail, hence we want to ensure that
             we are resetting the e to be the head of the list, in this case it
             will always be the list_prev. */
          e = list_prev (e);
          free_pcb (p);
        }
      else
        {
          sema_up (&p->semaphore);
        }
    }
}

/* Returns the number of threads currently in the ready list. */
size_t
threads_ready (void)
{
  return list_size (&ready_list);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void)
{
  struct thread *t = thread_current ();

  if (thread_mlfqs)
    {
      if (t != idle_thread)
        {
          t->recent_cpu += TO_FIXED_POINT(1);

          /* Add current thread to changed thread list if not in changed thread list */
          if (changed > 0)
            {
              /* Checks if the thread ran in last tick is still the same one */
              if (changed_threads[changed - 1] != t)
                {
                  changed_threads[changed] = t;
                  changed ++;
                }
            }
          else
          /* Current thread isn't in the list yet */
            {
              changed_threads[changed] = t;
              changed ++;
            }
        }

      if (timer_ticks () % TIMER_FREQ == 0)
        {
          /* Calculates load average. */
          thread_set_load_avg ();

          /* Iterates though all_list and updates recent_cpu. Updates
             priorities if recent_cpu has changed. */
          thread_foreach (thread_calculate_recent_cpu_of, NULL);

          /* Assuming that at least 1 priority in ready list is updated, need
             to sort. */
          if (!list_empty (&ready_list))
            {
              list_sort_desc_pri (&ready_list);
              thread_check_if_yield ();
            }
          changed = 0;
        }

      else if (timer_ticks () % 4 == 0)
        {
          /* The ticks before this might have different threads running, hence we
             iterate through the changed_threads list to ensure that we update all
             threads that have their cpu changed in the past 4 ticks. */
          for (int i = 0; i < changed; i++)
            {
              struct thread *changed_t = changed_threads[i];
              if (changed_t != NULL)
                changed_t -> base_priority = thread_get_priority_mlfqs (changed_t);
            }
          changed = 0;
          thread_check_if_yield ();
        }
    }

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();

}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  struct pcb *p = malloc (sizeof (struct pcb));

  /* We know that main thread doesn't call thread_create so main thread will
     have its own_pcb set as NULL. */
  if (p == NULL)
    {
      t->own_pcb = NULL;
    }
  else
    {
      struct thread *curr_t = thread_current ();
      p->exit_status = 0;
      p->load_status = 0;
      p->parent = curr_t;
      p->wait = false;
      p->pid = tid;
      t->own_pcb = p;
      sema_init (&p->semaphore, 0);
      list_push_back (&curr_t->child_pcb_list, &p->childelem);
    }

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack'
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  /* Check if current thread still has highest priority. */
  thread_check_if_yield ();
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_desc_pri (&ready_list, &t->elem, NULL);
  t->status = THREAD_READY;

  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

  if (strcmp (thread_current()->name, "main") == 0) {
    free_all_child_pcb (thread_current());
  }

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  struct thread *curr_t = thread_current ();
  curr_t->status = THREAD_DYING;
  /* Remove from changed threads list */
  for (int i = 0; i < changed; i++)
    {
      if (changed_threads[changed] == curr_t)
        changed_threads[changed] = NULL;
    }
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    list_insert_desc_pri (&ready_list, &cur->elem, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Comparator used to sort the list in descending order of priority.
   Returns true if thread a has higher priority than thread b. */
static bool
desc_pri_func (const struct list_elem *a,
               const struct list_elem *b, void *aux UNUSED)
{
  struct thread *th_a;
  struct thread *th_b;

  th_a = list_entry (a, struct thread, elem);
  th_b = list_entry (b, struct thread, elem);

  ASSERT (is_thread (th_a));
  ASSERT (is_thread (th_b));

  return thread_get_priority_of (th_a) > thread_get_priority_of (th_b);
}

/* Insert ELEM in descending order of priority. */
void
list_insert_desc_pri (struct list *list, struct list_elem *elem,
                      void *aux UNUSED)
{
  list_insert_ordered (list, elem, desc_pri_func, aux);
}

/* Sorts list of threads in descending order of priority. */
void
list_sort_desc_pri (struct list *list)
{
  list_sort (list, desc_pri_func, NULL);
}

/* Checks if current thread needs to yield to a higher priority thread. */
void
thread_check_if_yield (void)
{
  if (!list_empty (&ready_list))
    {
      struct thread *high_t = list_entry (list_front (&ready_list),
                                          struct thread, elem);
      if (thread_get_priority_of (high_t) > thread_get_priority ())
        {
          if (!intr_context ())
            thread_yield();
          else
            intr_yield_on_return ();
        }
    }
}

/* Calculates the mlfqs priority of a thread based on recent_cpu and nice. */
static int
thread_get_priority_mlfqs (struct thread *t)
{
  int p = TO_INTEGER_ROUNDING_TO_ZERO (
            SUBTRACT (
              SUBTRACT (
                TO_FIXED_POINT (PRI_MAX),
                (DIVIDE_WHEN_N_NOT_FIXED_POINT (t->recent_cpu, 4))),
              MULTIPLY_WHEN_N_NOT_FIXED_POINT (TO_FIXED_POINT (t->nice), 2)));
  return clamp (p, 63, 0);
}

/* Sets the current thread's priority to new_priority. */
void
thread_set_priority (int new_priority)
{
  struct thread *curr_t = thread_current ();
  if (thread_mlfqs)
    {
      new_priority = clamp(new_priority, 63, 0);
    }
  curr_t->base_priority = new_priority;
  /* No need to sort the ready list as this is only calculating the current
     thread, which isn't in the ready list. */
  thread_check_if_yield ();
}

/* Returns the effective priority of a particular thread. */
int
thread_get_priority_of (struct thread *t)
{
  int max_priority = 0;
  enum intr_level old_level;

  ASSERT (is_thread (t));

  if (thread_mlfqs)
    return t->base_priority;

  if (list_empty (&t->held_locks_list))
    return t->base_priority;

  old_level = intr_disable ();

  /* Goes through every lock that *t is holding. */
  for (struct list_elem *e = list_begin (&t->held_locks_list);
       e != list_end (&t->held_locks_list);
       e = list_next (e))
    {
      struct lock *held_lock = list_entry (e, struct lock, held_locks_elem);
      struct list *waiters = &held_lock->semaphore.waiters;

      /* Does nothing if that lock has no waiters, i.e. potential donors. */
      if (!list_empty (waiters))
        {
          /* Otherwise, updates the 'high score' of priorities amongst all
             locks if needed. */
          struct thread *max_donor = list_entry (list_front (waiters),
                                                 struct thread, elem);
          if (t != max_donor)
            {
              int max_priority_of_lock = thread_get_priority_of (max_donor);
              max_priority = (max_priority_of_lock > max_priority) ?
                             max_priority_of_lock : max_priority;
            }
        }
    }

  intr_set_level (old_level);

  /* Returns base_priority if no donors or if all donors do not exceed
     base_priority. */
  return (max_priority > t->base_priority) ?
         max_priority : t->base_priority;
}

/* Returns the current's priority. */
int
thread_get_priority (void)
{
  struct thread *curr_t = thread_current ();
  return thread_get_priority_of (curr_t);
}


/* Sets the current thread's nice value to NICE. Recalculates priority and
   checks if thread's priority is still highest. */
void
thread_set_nice (int nice)
{
  if (nice <= 20 && nice >= -20)
    {
      struct thread *curr_t = thread_current ();
      curr_t->nice = nice;
      curr_t->base_priority = thread_get_priority_mlfqs (curr_t);
      /* No need to sort the ready list as this is only calculating the current
         thread, which isn't in the ready list. */
      thread_check_if_yield ();
    }
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void)
{
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void)
{
  int la = MULTIPLY_WHEN_N_NOT_FIXED_POINT (load_avg, 100);
  la = TO_INTEGER_ROUNDING_TO_NEAREST (la);
  return la;
}

/* Updates the load_avg value of the system. */
static void
thread_set_load_avg (void)
{
  size_t length_of_readies = list_size (&ready_list);
  if (thread_current () != idle_thread)
    length_of_readies++;
  load_avg = ADD (
               MULTIPLY (
                 DIVIDE (TO_FIXED_POINT (59), TO_FIXED_POINT (60)),
                 load_avg),
               MULTIPLY_WHEN_N_NOT_FIXED_POINT (
                 DIVIDE (TO_FIXED_POINT (1), TO_FIXED_POINT (60)),
                 length_of_readies));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void)
{
  struct thread *curr_t = thread_current();
  return TO_INTEGER_ROUNDING_TO_NEAREST(
           MULTIPLY_WHEN_N_NOT_FIXED_POINT(curr_t->recent_cpu, 100));
}

/* Calculates recent_cpu of given thread, also checks if value has changed or
   not to see if t's priority needs to be recalculated. */
static void
thread_calculate_recent_cpu_of (struct thread *t, void *aux UNUSED)
{
  int fixed_load_avg = MULTIPLY_WHEN_N_NOT_FIXED_POINT (load_avg, 2);
  int fixed_add = ADD_WHEN_N_NOT_FIXED_POINT (fixed_load_avg, 1);
  int rc = ADD_WHEN_N_NOT_FIXED_POINT (
             MULTIPLY (DIVIDE (fixed_load_avg, fixed_add), t->recent_cpu),
             t->nice);

  /* Recalculates t's priority if recent_cpu changes. */
  if (rc != t->recent_cpu)
    {
      t->recent_cpu = rc;
      t->base_priority = thread_get_priority_mlfqs (t);
    }
}

static int
clamp(int given, int high, int low)
{
  if (given > high)
    {
      return high;
    }
  if (given < low)
    {
      return low;
    }
  return given;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;)
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)

{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;

  if (strcmp (t->name, "main") == 0)
    {
      t->own_pcb = NULL; // never need to use it
    }

  if (!thread_mlfqs)
    {
      t->base_priority = priority;
    }
  else
    {
      /* When first thread is being created. */
      if (strcmp (t->name, "main") == 0)
        {
          t->nice = 0;
          t->recent_cpu = 0;
          t->base_priority = 0;
        }
      else
        {
          /* If thread has a parent. */
          struct thread *curr_t = thread_current();
          if (curr_t != t)
            {
              t->nice = curr_t->nice;
              t->recent_cpu = curr_t->recent_cpu;
              t->base_priority = curr_t->base_priority;
            }
        }
    }
  sema_init (&t->wait_for_child_load_semaphore, 0);
  list_init (&t->held_locks_list);
  list_init (&t->child_pcb_list);

  list_init (&t->file_fd_table);
  list_init (&t->file_mmap_table);

  lock_init (&t->page_table_lock);

  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

void
thread_check_ticks (struct thread *thread)
{
  ASSERT (thread->status == THREAD_BLOCKED);
  if (thread != idle_thread) {
    if (thread->current_ticks > 0) {

      /*Reduces current thread's tick every tick called by tick_interrupt */
      thread->current_ticks -= 1;

      /* When thread has finished sleeping, it gets removed from the sleeping
         list and gets unblocked */
      if (thread->current_ticks <= 0) {
        list_remove (&thread->sleepingelem);
        thread_unblock(thread);
      }
    }
  }
}


/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
