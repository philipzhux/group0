#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "pagedir.h"
#include "threads/pte.h"

static void syscall_handler(struct intr_frame*);
bool validate_single(void* addr);
bool validate_args(void* addr, size_t size);
bool validate_str(void* ptr);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */
  
  // validate esp
  if(!validate_args((f->esp), sizeof(uint32_t))) {
      f->eax = -1;
      printf("%s: exit(%d)\n", thread_current()->pcb->process_name, -1);
      process_exit();
  }

  if (args[0] == SYS_EXIT) {
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
      f->eax = args[1] + 1;
  }

  
}

bool validate_single(void* addr) {
  if(addr > PHYS_BASE) return false;
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

bool validate_str(void* ptr) {
    char* s = (char *) ptr;
    while(1) {
        if (!validate_single(s)) {
            return false;
        } else if (*(s++) == '\0') {
            return true;
        }
    }
}