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

  struct list thread_list;
  int stack_page_cnt;
  struct lock
      master_lock; /* Lock used for thread_list, file_desc_list, user locks and semaphores list */
  struct list
      join_status_list; // list of join_statuses for threads in this process; only holds unfinished or unjoined threads
  struct condition exit_cond_var; //
};

typedef struct join_status {
  tid_t tid;       // tid of thread; set by start_pthread to TID_ERROR if thread failed to start
  bool was_joined; // true if a thread has called pthread_join on this thread
  struct semaphore
      join_sema; // semaphore for waiting on thread to start (in pthread_execute), and waiting on thread to finish (in pthread_join)
  struct list_elem elem; // list construct
} join_status_t;

typedef struct thread_init {
  // used by process_execute
  char* file_name;           // file name of program running
  proc_status_t* status_ptr; // pointer to process status of starting process

  // userd for pthread_execute, start_pthread, pthread_join,
  stub_fun sf;                // stub function for starting thread
  pthread_fun tf;             // function to run for starting thread
  void* arg;                  // args passed by user for starting thread
  struct process* pcb;        // pointer to pcb
  join_status_t* join_status; // pointer to join status of starting thread
} thread_init_t;

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(int status);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);
void release_proc_status(proc_status_t* status, bool parent);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
