#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "pagedir.h"
#include "threads/pte.h"
#include "threads/malloc.h"

static void syscall_handler(struct intr_frame*);
bool validate_single(void* addr);
bool validate_args(void* addr, size_t size);
bool validate_str(char* ptr);
void validate_fail(struct intr_frame*);
void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a systemst call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */
  
  // validate esp
  if(!validate_args((f->esp), sizeof(uint32_t))) {
    validate_fail(f);
  }

  if (args[0] == SYS_EXIT) {
    if (!validate_args(&args[1], sizeof(uint32_t))) {
      validate_fail(f);
    }

    struct list* child_list = thread_current()->pcb->child_status_list;
    for (struct list_elem* e = list_begin(child_list); e != list_end(child_list); e = list_next(e)) {
        proc_status_t * status = list_entry(e, proc_status_t, elem);
        release_proc_status(status, true);
    }
    free(child_list);
    thread_current()->pcb->own_status->exit_status = args[1];
    sema_up(&thread_current()->pcb->own_status->wait_sema);
    release_proc_status(thread_current()->pcb->own_status, false);

    f->eax = args[1];
    printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
    process_exit();
  }
  else if(args[0] == SYS_WRITE)
  {
    // TODO: temporary implementation. Add validation + support for arbitrary fd later.
    if(args[1] == STDOUT_FILENO)
    {
      printf("%.*s", (unsigned int) args[3], (char*) args[2]);
    } 
  }
  else if (args[0] == SYS_PRACTICE) {
    if (!validate_args(&args[1], sizeof(uint32_t))) {
      validate_fail(f);
    }
    f->eax = args[1] + 1;
  }
  else if (args[0] == SYS_EXEC) {
    if (!validate_args(&args[1], sizeof(uint32_t))) {
      validate_fail(f);
    }
    if (!validate_str((char *) args[1])) {
      validate_fail(f);
    }
    f->eax = process_execute((char *) args[1]);
  }

  
}

bool validate_single(void* addr) {
  if(addr >= PHYS_BASE) return false;
  /* translate addr into page table entry */
  uint32_t* current_pd = active_pd();
  void* pg = pagedir_get_page(current_pd, addr);
  return pg != NULL;
}

bool validate_args(void* addr, size_t size) {
  void* cur_addr = (void*)pg_round_down(addr);
  while(cur_addr < addr + size)
  {
    if(!validate_single(cur_addr)) return false;
    cur_addr += PGSIZE;
  }
  return true;
}

bool validate_str(char* ptr) {
  while(1) {
    if (!validate_single((void*)ptr)) {
      return false;
    } else if (*(ptr++) == '\0') {
      return true;
    }
  }
}

void validate_fail(struct intr_frame* f) {
  f->eax = -1;
  printf("%s: exit(%d)\n", thread_current()->pcb->process_name, -1);
  process_exit();
}