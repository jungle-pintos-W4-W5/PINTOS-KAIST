#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static void check_valid_ptr (void *ptr);
static void check_valid_fd (int fd);
static int allocate_fd(struct thread* t);
static bool lesser_fd(struct list_elem *a, struct list_elem *b, void *aux);
static struct file_descriptor* find_fd (struct thread* t, int fd);

static void halt (void);
static void exit (int status);
static int write (int fd, const void *buffer, unsigned size);
static bool create (const char *file, unsigned initial_size);
static int open (const char *file_name);
static void close (int fd);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned size);
static int write (int fd, const void *buffer, unsigned size);


/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	lock_init(&filesys_lock);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	int syscall_number = f->R.rax;

	uint64_t arg1 = f->R.rdi;	
	uint64_t arg2 = f->R.rsi;	
	uint64_t arg3 = f->R.rdx;	

	switch (syscall_number) {
		case SYS_EXIT:
			exit(arg1);
			break;
		case SYS_HALT:
			power_off();
			break;
		case SYS_CREATE:
			f->R.rax = create(arg1, arg2);
			break;
		case SYS_OPEN:
			f->R.rax = open(arg1);
			break;
		case SYS_CLOSE:
			close(arg1);
			break;
		case SYS_READ:
			break;
		case SYS_WRITE:
			f->R.rax = write(arg1, arg2, arg3);
			break;
		case SYS_FILESIZE:
			break;
		default:
			thread_exit ();
	}
}

static void exit (int status) {
	thread_current()->exit_status = status;
	thread_exit();
}

static bool create (const char *file, unsigned initial_size) {
	check_valid_ptr(file);

	lock_acquire(&filesys_lock);
	bool success = filesys_create(file, initial_size);
	lock_release(&filesys_lock);

	return success;
}

static int open (const char *file_name) {
	check_valid_ptr(file_name);
	
	lock_acquire(&filesys_lock);

	struct file* file = filesys_open(file_name);
	if (file == NULL) 
		return -1;
	lock_release(&filesys_lock);

	struct thread* curr = thread_current();

	struct file_descriptor* fd = malloc(sizeof(struct file_descriptor));
	fd->fd_val = allocate_fd(curr);
	fd->fd_file = file;
	list_push_back(&curr->fd_table, &fd->fd_elem);

	return fd->fd_val;
}

static void close (int fd) {
	check_valid_fd(fd);

	struct thread *cur = thread_current();

	lock_acquire(&filesys_lock);
	
	struct file_descriptor *real_fd = find_fd(cur, fd);
	if (real_fd == NULL) 
		return;
	
	struct file* file = real_fd->fd_file;
	if (file == NULL) 
		return;

	list_remove(&real_fd->fd_elem);

	file_close(file);
	lock_release(&filesys_lock);

	free(real_fd);
}

static int write (int fd, const void *buffer, unsigned size) {
	check_valid_ptr(buffer);

	if (fd == 1) {
		putbuf(buffer, size);
		return size;
	}
}

static void check_valid_ptr (void *ptr) {
	if (ptr == NULL)	// if invalid ptr
		exit(-1);
	
	if (is_kernel_vaddr(ptr))	// if is not user vaddr
		exit(-1);

	if (pml4_get_page(thread_current()->pml4, ptr) == NULL)	// if is not mapped
		exit(-1);
}

static void check_valid_fd (int fd) {
	if (fd < MIN_FD || fd > MAX_FD)
		exit(-1);
}
/* ########### HELPER FUNCTIONS ############## */

// returns available fd
static int allocate_fd(struct thread* t) {
	struct list *files = &t->fd_table;
	int fd = MIN_FD;

	if (!list_empty(files)) {
		list_sort(files, lesser_fd, NULL); // fd값 낮은 순

		struct list_elem *e;
		for (e = list_front(files); e != list_end(files);) {
			struct list_elem *next = list_next(e);
			struct file_descriptor *curr = list_entry(e, struct file_descriptor, fd_elem);

			if (curr->fd_val == fd) {
				fd += 1;
				e = next;
			} else break;	// curr->fd_val > fd ==> 해당 fd 값 가능
		}
	}
	return fd;
}

static bool lesser_fd(struct list_elem *a, struct list_elem *b, void *aux) {
	struct file_descriptor *fd_a = list_entry(a, struct file_descriptor, fd_elem);
	struct file_descriptor *fd_b = list_entry(b, struct file_descriptor, fd_elem);
	return fd_a->fd_val < fd_b->fd_val;
}

static struct file_descriptor* find_fd (struct thread* t, int fd) {
	struct list *files = &t->fd_table;

	if (!list_empty(files)) {
		struct list_elem *e;
		for (e = list_front(files); e != list_end(files);) {
			struct list_elem *next = list_next(e);
			struct file_descriptor *curr = list_entry(e, struct file_descriptor, fd_elem);

			if (curr->fd_val == fd) 
				return curr;
			e = next;
		}
	}

	return NULL;
}