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

static void halt (void);
static void exit (int status);
static int write (int fd, const void *buffer, unsigned size);
static bool create (const char *file, unsigned initial_size);

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
		case SYS_WRITE:
			f->R.rax = write(arg1, arg2, arg3);
			break;
		case SYS_CREATE:
			f->R.rax = create(arg1, arg2);
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
	bool success = filesys_create(file, initial_size);
	return success;
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
