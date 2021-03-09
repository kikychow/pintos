#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

/* Contains processed information needed to setup the stack. */
struct arguments
  {
    char **argv;  /* Array of parsed argument tokens. */
    int argc;     /* Number of arguments. */
  };

/* Lock used to synchronise file system. */
struct lock filesys_lock;

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
