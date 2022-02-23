#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <console.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "pagedir.h"
#include "threads/pte.h"
#include "threads/malloc.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
extern struct lock file_lock;
extern void putbuf(const char* buffer, size_t n);

static void syscall_handler(struct intr_frame*);
bool validate_single(void* addr);
bool validate_args(void* addr, size_t size);
bool validate_str(char* ptr);
void validate_fail(struct intr_frame*);
void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }
file_desc_t* find_file(struct process *pcb, int fd);

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
    f->eax = args[1];
    process_exit(args[1]);
  } else if (args[0] == SYS_PRACTICE) {
    if (!validate_args(&args[1], sizeof(uint32_t))) {
      validate_fail(f);
    }
    f->eax = args[1] + 1;
  } else if (args[0] == SYS_EXEC) {
    if (!validate_args(&args[1], sizeof(uint32_t))) {
      validate_fail(f);
    }
    if (!validate_str((char *) args[1])) {
      validate_fail(f);
    }
    f->eax = process_execute((char *) args[1]);
  } else if (args[0] == SYS_WAIT) {
    if (!validate_args(&args[1], sizeof(uint32_t))) {
      validate_fail(f);
    }
    f->eax = process_wait((pid_t) args[1]);
  } else if (args[0] == SYS_HALT) {
    shutdown_power_off();
  } else if (args[0] == SYS_CREATE) {
    if (!validate_args(&args[1], sizeof(char*) + sizeof(unsigned int))) {
      validate_fail(f);
    }
    if (!validate_str((char *) args[1])) {
      validate_fail(f);
    }

    lock_acquire(&file_lock);
    f->eax = filesys_create((char *) args[1], (unsigned int) args[2]);
    lock_release(&file_lock);

  } else if (args[0] == SYS_OPEN) {
    if (!validate_args(&args[1], sizeof(char*))) {
      validate_fail(f);
    }
    if (!validate_str((char *) args[1])) {
      validate_fail(f);
    }
    f->eax = -1;
    struct process *pcb = thread_current()->pcb;

    lock_acquire(&file_lock);
    struct file* file_ptr = filesys_open((char *) args[1]);
    lock_release(&file_lock);

    if(file_ptr) {
      file_desc_t *fdesc  = (file_desc_t *) malloc(sizeof(file_desc_t));
      fdesc->fd = pcb->file_desc_count++;
      fdesc->file = file_ptr;
      list_push_back(pcb->file_desc_list, &fdesc->elem);
      f->eax = fdesc->fd;
    }
    
  } else if (args[0] == SYS_REMOVE) {
    if (!validate_args(&args[1], sizeof(char*))) {
      validate_fail(f);
    }
    if (!validate_str((char *) args[1])) {
      validate_fail(f);
    }
    
    lock_acquire(&file_lock);
    f->eax = filesys_remove((char *) args[1]); 
    lock_release(&file_lock);

  } else if (args[0] == SYS_CLOSE) {
     if (!validate_args(&args[1], sizeof(int))) {
      validate_fail(f);   
    }
    
    file_desc_t* filedesc = find_file(thread_current()->pcb, args[1]);
    if (filedesc == NULL) {
      return;
    }
    
    lock_acquire(&file_lock);
    file_close(filedesc->file);
    lock_release(&file_lock);
    
    list_remove(&filedesc->elem);

  } else if (args[0] == SYS_FILESIZE) {
     if (!validate_args(&args[1], sizeof(int))) {
      validate_fail(f);
    }
    file_desc_t* filedesc = find_file(thread_current()->pcb, args[1]);
    if (filedesc == NULL) {
      f->eax = -1;
      return;
    }
    
    lock_acquire(&file_lock);
    f->eax = file_length(filedesc->file);
    lock_release(&file_lock);

  } else if (args[0] == SYS_READ) {
    if (!validate_args(&args[1], sizeof(int) + sizeof(void*) + sizeof(unsigned int))) {
      validate_fail(f);
    }
    if (!validate_args((void *) args[2], (size_t) args[3])) {
      validate_fail(f);
    }

    if(args[1] == STDIN_FILENO) {
      for(int i = 0; i < (int) args[3]; i++)
      {
        ((char*)args[2])[i] = input_getc();
      }
      f->eax = args[3];
      return;
    }
    
    file_desc_t* filedesc = find_file(thread_current()->pcb, args[1]);
    if(filedesc == NULL)
    {
      f->eax = -1;
      return;
    }
    lock_acquire(&file_lock);
    f->eax = file_read(filedesc->file, (void*) args[2], (off_t) args[3]);
    lock_release(&file_lock);

  } else if(args[0] == SYS_WRITE) {
    if (!validate_args(&args[1], sizeof(int) + sizeof(void*) + sizeof(unsigned int))) {
      validate_fail(f);
    }
    if (!validate_args((void *) args[2], (size_t) args[3])) {
      validate_fail(f);
    }

    if(args[1] == STDOUT_FILENO) {
      putbuf((char*) args[2], (unsigned int) args[3]);
      f->eax = args[3];
      return;
    }

    file_desc_t* filedesc = find_file(thread_current()->pcb, args[1]);
    if(filedesc == NULL)
    {
      f->eax = 0;
      return;
    }
    lock_acquire(&file_lock);
    f->eax = file_write(filedesc->file,(void*) args[2],(off_t) args[3]);
    lock_release(&file_lock);
    
  } else if (args[0] == SYS_SEEK) {
    if (!validate_args(&args[1], sizeof(int) + sizeof(int))) {
      validate_fail(f);
    }
    
    lock_acquire(&file_lock);
    file_desc_t* filedesc = find_file(thread_current()->pcb, args[1]);
    
    if (filedesc == NULL) {
      return;
    }

    file_seek(filedesc->file, (off_t) args[2]);
    lock_release(&file_lock);
    
  } else if (args[0] == SYS_TELL) {
    if (!validate_args(&args[1], sizeof(int))) {
      validate_fail(f);
    }

    file_desc_t* filedesc = find_file(thread_current()->pcb, args[1]);
    if(filedesc == NULL)
    {
      f->eax = -1;
      return;
    }

    lock_acquire(&file_lock);
    f->eax = file_tell(filedesc->file);
    lock_release(&file_lock);

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
  process_exit(-1);
}

file_desc_t* find_file(struct process *pcb, int fd) {
  file_desc_t* filedesc = NULL;
  for (struct list_elem* e = list_begin(pcb->file_desc_list); e != list_end(pcb->file_desc_list); e = list_next(e)) {
    file_desc_t *tmp = list_entry(e, file_desc_t, elem);
    if (tmp->fd == fd) {
      filedesc = tmp;
      break;
    }
  }
  return filedesc;
}