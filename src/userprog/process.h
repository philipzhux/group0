#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stdint.h>
#include <list.h>
#include "threads/thread.h"
#include "filesys/file.h"

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

typedef struct proc_status {
  struct list_elem elem;      // PintOS list construct
  pid_t pid;                  // PID of the process
  struct process* parent_pcb; // PCB of parent
  int exit_status;            // Exit status of the process
  int ref_count;              // # of references to this struct
  struct lock ref_lock;       // lock for ref_count
  struct semaphore wait_sema; // synchronization for parent/child in exec() and wait()
} proc_status_t;

typedef struct file_desc {
  struct list_elem elem; // PintOS list construct.
  int fd;                // file descriptor number.
  struct file* file;     // file pointer to call library functions.
} file_desc_t;

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */
  struct list* child_status_list;
  proc_status_t* own_status;
  struct list* file_desc_list; /* Pointer to list of file descriptions. */
  uint32_t file_desc_count;    /* Starts at 2, and increases when files are opened. */
  struct file* exec_file;      /* File pointer to currently executing file. */
};

typedef struct thread_init {
  proc_status_t* status_ptr;
  char* file_name;
} thread_init_t;

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(int status);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);
void release_proc_status(proc_status_t* status, bool parent);
#endif /* userprog/process.h */
