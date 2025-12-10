#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
bool lazy_load_segment (struct page *page, void *aux);

struct fork_aux {
    struct thread *parent;
	struct child *child_info;
    struct semaphore fork_sema;
	struct intr_frame parent_if;
    bool fork_success;
};

struct init_aux {
    char *fn_copy;
    struct child *child_info;
};

struct lazy_aux {
	struct file* file;
	off_t ofs;
	size_t page_read_bytes;
	size_t page_zero_bytes;
};

#endif /* userprog/process.h */
