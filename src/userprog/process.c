#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp, thread_init_t* args);

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  /* Kill the kernel if we did not succeed */
  ASSERT(success);

  t->pcb->child_status_list = (struct list*)malloc(sizeof(struct list));
  list_init(t->pcb->child_status_list);
  t->pcb->file_desc_list = (struct list*)malloc(sizeof(struct list));
  list_init(t->pcb->file_desc_list);
  list_init(&t->pcb->join_status_list);
  lock_init(&t->pcb->master_lock);
  cond_init(&t->pcb->exit_cond_var);
  join_status_t* main_status = malloc(sizeof(join_status_t));
  sema_init(&main_status->join_sema, 0);
  main_status->was_joined = false;
  main_status->tid = t->tid;
  t->join_status = main_status;
  list_push_front(&t->pcb->join_status_list, &main_status->elem);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;
  int prog_name_len = strcspn(file_name, " ");
  char prog_name[prog_name_len + 1]; // Program name.
  strlcpy(prog_name, file_name, prog_name_len + 1);

  proc_status_t* status_ptr = (proc_status_t*)malloc(sizeof(proc_status_t));
  status_ptr->pid = -1;
  status_ptr->parent_pcb = thread_current()->pcb;
  sema_init(&(status_ptr->wait_sema), 0);
  lock_init(&(status_ptr->ref_lock));
  status_ptr->ref_count = 2;
  thread_init_t* attr = (thread_init_t*)malloc(sizeof(thread_init));

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  attr->file_name = fn_copy;
  attr->status_ptr = status_ptr;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(prog_name, PRI_DEFAULT, start_process, attr);
  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);

  // wait for child to load
  sema_down(&status_ptr->wait_sema);
  int pid = status_ptr->pid;
  free(attr); // attr no longer in use
  if (pid == -1) {
    /* loading child fails
    need to clean up the status struct for the "expected" child */
    free(status_ptr);
    return -1;
  }
  // add child to list
  list_push_back(thread_current()->pcb->child_status_list, &status_ptr->elem);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* attr_) {
  thread_init_t* attr = (thread_init_t*)attr_;
  char* file_name = attr->file_name;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp);

    asm volatile("fninit; fsave (%0)" : : "g"(&if_.fpu));
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  if (success) {
    attr->status_ptr->pid = t->tid;
    t->pcb->own_status = attr->status_ptr;
    t->pcb->child_status_list = (struct list*)malloc(sizeof(struct list));
    t->pcb->file_desc_list = (struct list*)malloc(sizeof(struct list));
    list_init(t->pcb->child_status_list);
    list_init(t->pcb->file_desc_list);
    list_init(&(t->pcb->thread_list));
    list_init(&t->pcb->join_status_list);
    t->pcb->stack_page_cnt = 1;
    t->pcb->file_desc_count = 2;
    cond_init(&t->pcb->exit_cond_var);
    lock_init(&t->pcb->master_lock);

    t->is_exiting = false;
    // allocate and initialize a join_status for the main thread
    join_status_t* main_status = malloc(sizeof(join_status_t));
    sema_init(&main_status->join_sema, 0);
    main_status->was_joined = false;
    main_status->tid = t->tid;
    t->join_status = main_status;
    list_push_front(&t->pcb->join_status_list, &main_status->elem);

    // put main thread onto thread_list
    list_push_front(&t->pcb->thread_list, &t->proc_thread_list_elem);
  }

  sema_up(&(attr->status_ptr->wait_sema));
  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  if (!success) {
    thread_exit();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct process* pcb = thread_current()->pcb;
  struct list* lst = pcb->child_status_list;
  struct list_elem* e;
  proc_status_t* status = NULL;
  int exit_status = -1;

  for (e = list_begin(lst); e != list_end(lst); e = list_next(e)) {
    proc_status_t* tmp = list_entry(e, proc_status_t, elem);
    if (tmp->pid == child_pid) {
      status = tmp;
      break;
    }
  }
  if (status == NULL) {
    return exit_status;
  }
  sema_down(&status->wait_sema);
  exit_status = status->exit_status;
  release_proc_status(status, true);
  return exit_status;
}

/* Free the current process's resources. */
void process_exit(int status) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }
  
  // check that no other thread has already called process_exit
  lock_acquire(&cur->pcb->master_lock);
  if (cur->is_exiting) {
    lock_release(&cur->pcb->master_lock);
    pthread_exit();
    NOT_REACHED();
  }

  // wait on all other threads to die
  while(list_size(&cur->pcb->thread_list) > 1) {
    cond_wait(&cur->pcb->exit_cond_var, &cur->pcb->master_lock);
  }
  lock_release(&cur->pcb->master_lock);
  
  // free the join status list
  while (!list_empty(&cur->pcb->join_status_list)) {
    struct join_status * status = list_entry(list_pop_front(&cur->pcb->join_status_list), struct join_status, elem);
    free(status);
  }

  // clean up child_status_list
  struct list* child_list = cur->pcb->child_status_list;
  while (!list_empty(child_list)) {
    struct proc_status * status = list_entry(list_pop_front(child_list), struct proc_status, elem);
    release_proc_status(status, true);
  }
  free(child_list);

  // clean up file_desc_list
  struct list* file_list = cur->pcb->file_desc_list;
  if (!list_empty(file_list)) {
    file_desc_t* prev = list_entry(list_begin(file_list), file_desc_t, elem);
    for (struct list_elem* e = list_next(list_begin(file_list)); e != list_end(file_list);
         e = list_next(e)) {
      file_close(prev->file);
      free(prev);
      prev = list_entry(e, file_desc_t, elem);
    }
    free(prev);
  }
  free(cur->pcb->file_desc_list);

  file_close(cur->pcb->exec_file);
  //set own exit status
  cur->pcb->own_status->exit_status = status;
  sema_up(&cur->pcb->own_status->wait_sema);
  release_proc_status(cur->pcb->own_status, false);
  printf("%s: exit(%d)\n", cur->pcb->process_name, status);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
      cur->pcb->pagedir to NULL before switching page directories,
      so that a timer interrupt can't switch back to the
      process page directory.  We must activate the base page
      directory before destroying the process's page
      directory, or our active page directory will be one
      that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);
static void parse_args(char* filename, void** esp);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  int prog_name_len = strcspn(file_name, " ");
  char prog_name[prog_name_len + 1]; // Program name.
  strlcpy(prog_name, file_name, prog_name_len + 1);
  // prog_name[prog_name_len] = '\0';

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(prog_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", prog_name);
    goto done;
  }
  t->pcb->exec_file = file;
  file_deny_write(file);

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", prog_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;
  parse_args(file_name, esp);

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if (!success) {
    file_close(file);
  }
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Parse the filename for command line arguments,
   then push them onto the stack appropriately. */
void parse_args(char* filename, void** esp) {
  // push strings in forward order (easiest to implement)
  int argc = 0;
  char* save_ptr = NULL;
  char* token;
  for (token = strtok_r(filename, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr)) // see lib/string.c:strtok_r()
  {
    int len = strlen(token);
    *esp -= (len + 1);
    strlcpy(*esp, token, len + 1);
    argc++;
  }

  // push argv[0...argc]
  char** argv_start = *esp - sizeof(char*) * (argc + 1);
  size_t cur_length = 0;
  for (int i = argc - 1; i >= 0; i--) {
    argv_start[i] = *esp + cur_length;
    cur_length += strlen(*esp + cur_length) + 1;
  }
  argv_start[argc] = NULL;
  *esp = argv_start;

  // push padding
  size_t pad_size = ((unsigned int)argv_start - sizeof(char**) - sizeof(int)) % 16;
  *esp -= pad_size;
  memset(*esp, 0, pad_size); // may not be needed

  // push argv, argc, fake return addr
  *esp -= sizeof(char**);
  *(char***)*esp = argv_start;
  *esp -= sizeof(int);
  *(int*)*esp = argc;
  *esp -= 4;
  *(int*)*esp = 0;
}

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

void release_proc_status(proc_status_t* status, bool parent) {
  int should_free;
  lock_acquire(&(status->ref_lock));
  status->ref_count -= 1;
  should_free = (status->ref_count == 0);
  lock_release(&(status->ref_lock));
  if (should_free) {
    // free stuff in proc_status
    if (parent)
      list_remove(&status->elem);
    free(status);
  }
}
/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void), void** esp, thread_init_t* args) {
  struct thread* t = thread_current();

  // set eip
  *eip = (void*)(args->sf);

  // allocate thread stack and set esp
  uint8_t* kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  bool success = false;

  if (kpage != NULL) {
    void* addr = PHYS_BASE - PGSIZE;
    while (pagedir_get_page(t->pcb->pagedir, (uint32_t*)addr)) {
      if (addr == 0) {
        palloc_free_page(kpage);
        return false;
      }
      addr -= PGSIZE;
    }

    success = install_page(addr, kpage, true);
    if (success) {
      *esp = addr + PGSIZE;
      // t->pcb->stack_page_cnt++;
      t->saved_upage = addr;

      /* push args
      0x...8 [8] padding
      0x...4 [4] (void*) arg
      0x...0 [4] (pthread_fun)
      0x...c [4] fake rip */
      *esp -= 8;
      *((long*)*esp) = 0;

      *esp -= sizeof(void*);
      *((int**)*esp) = args->arg;

      *esp -= sizeof(pthread_fun);
      *((pthread_fun*)*esp) = args->tf;

      *esp -= sizeof(int);
      *((int*)*esp) = 0;
    } else
      palloc_free_page(kpage);
  }

  return success;
}

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf, pthread_fun tf, void* arg) {
  struct process* pcb = thread_current()->pcb;
  thread_init_t* start_pthread_args = malloc(sizeof(thread_init_t));
  start_pthread_args->sf = sf;
  start_pthread_args->tf = tf;
  start_pthread_args->arg = arg;
  start_pthread_args->pcb = pcb;
  start_pthread_args->join_status = malloc(sizeof(join_status_t));

  // init join status
  join_status_t* status = start_pthread_args->join_status;
  sema_init(&status->join_sema, 0);
  status->was_joined = false;

  // set name of thread
  char name[16];
  snprintf(name, 15, "%p", tf);

  thread_create(name, PRI_DEFAULT, start_pthread, start_pthread_args);
  sema_down(&status->join_sema);

  // handle join_status based on result
  if (status->tid == TID_ERROR) {
    free(status);
    return TID_ERROR;
  } else {
    return status->tid;
  }
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* args_) {
  thread_init_t* args = (thread_init_t*)args_;
  struct thread* t = thread_current();
  t->pcb = args->pcb;
  process_activate();

  // initialize interrupt frame
  struct intr_frame if_;
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  asm volatile("fninit; fsave (%0)" : : "g"(&if_.fpu));

  bool success = setup_thread(&if_.eip, &if_.esp, args);

  join_status_t* status = args->join_status;
  free(args);
  if (!success) {
    status->tid = TID_ERROR;
    sema_up(&status->join_sema);
    thread_exit(); // does not return
  } else {
    status->tid = t->tid;
    sema_up(&status->join_sema);
  }

  // push new thread onto thread_list and join status onto join_status_list
  lock_acquire(&t->pcb->master_lock);
  list_push_front(&t->pcb->thread_list, &t->proc_thread_list_elem);
  list_push_back(&t->pcb->join_status_list, &status->elem);
  lock_release(&t->pcb->master_lock);
  t->join_status = status;

  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid) {

  struct thread* t = thread_current();
  join_status_t* status = NULL;
  lock_acquire(&t->pcb->master_lock);

  for (struct list_elem* e = list_begin(&t->pcb->join_status_list);
       e != list_end(&t->pcb->join_status_list); e = list_next(e)) {
    struct join_status* tmp = list_entry(e, struct join_status, elem);
    if (tmp->tid == tid) {
      status = tmp;
    }
  }
  if (status == NULL || status->was_joined) {
    lock_release(&t->pcb->master_lock);
    return TID_ERROR;
  }
  status->was_joined = true;
  lock_release(&t->pcb->master_lock);

  sema_down(&status->join_sema);

  lock_acquire(&t->pcb->master_lock);
  list_remove(&status->elem);
  lock_release(&t->pcb->master_lock);
  free(status);
  return tid;
}

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  struct thread* t = thread_current();
  join_status_t* status = t->join_status;

  // free user stack
  void* kpage = pagedir_get_page(t->pcb->pagedir, t->saved_upage);
  palloc_free_page(kpage);
  pagedir_clear_page(t->pcb->pagedir, t->saved_upage); // clear page table entry

  lock_acquire(&t->pcb->master_lock);
  list_remove(&t->proc_thread_list_elem);
  // wake up thread that has joined on this
  lock_release(&t->pcb->master_lock);
  sema_up(&status->join_sema);

  // wake up thread if it is exiting process
  lock_acquire(&t->pcb->master_lock);
  if (list_size(&t->pcb->thread_list) == 1) {
    cond_signal(&t->pcb->exit_cond_var, &t->pcb->master_lock);
  }
  lock_release(&t->pcb->master_lock);

  thread_exit();
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {
  // wake up thread that has joined on this
  struct thread* t = thread_current();
  struct join_status* status = t->join_status;
  sema_up(&status->join_sema);

  // wake up thread if it is exiting process
  lock_acquire(&t->pcb->master_lock);
  
  while(list_size(&t->pcb->join_status_list) > 0) {
    if (list_size(&t->pcb->join_status_list) == 1) {
      struct join_status *temp = list_entry(list_begin(&t->pcb->join_status_list), struct join_status, elem);
      if(temp->was_joined || temp->tid == t->tid) break;
    }
    for (struct list_elem *e = list_begin(&t->pcb->join_status_list);  e != list_end(&t->pcb->join_status_list); e = list_next(e)) {
      struct join_status *tmp = list_entry(e, struct join_status, elem);
      if (!tmp->was_joined && tmp->tid != t->tid) {
        tmp->was_joined = true;
        list_remove(e);
        lock_release(&t->pcb->master_lock);
        sema_down(&tmp->join_sema);
        lock_acquire(&t->pcb->master_lock);
        break;
      }
    }
  }
  lock_release(&t->pcb->master_lock);
  process_exit(0);
}
