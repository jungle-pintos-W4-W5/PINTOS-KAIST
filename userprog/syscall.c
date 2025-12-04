#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "include/threads/synch.h"
#include "include/filesys/filesys.h"
#include "include/filesys/file.h"
#include "include/userprog/process.h"
#define EXIT_FAILURE -1
#define FD_MIN 2
void syscall_entry(void);
void syscall_handler(struct intr_frame*);

static void check_valid_ptr(const void* u_addr);
static void check_writeable_ptr(const void* u_addr);
static void check_valid_buffer(const void* u_addr, size_t size, bool check_write);
static bool fd_low_order(const struct list_elem* a_, const struct list_elem* b_, void* aux UNUSED);

static void halt(void);
static void exit(int);
static bool create(const char*, int32_t);
static bool remove(const char*);
static int open(const char*);
static void close(int);
static unsigned tell(int);
static void seek(int, unsigned);
static int filesize(int);
static int read(int, void*, unsigned);
static int write(int, void*, unsigned);

struct lock syslock;
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

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    lock_init(&syslock);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED)
{
    // TODO: Your implementation goes here.
    switch (f->R.rax) {
    case SYS_EXIT:
        exit(f->R.rdi);
        break;
    case SYS_HALT:
        halt();
        break;
    case SYS_OPEN:
        f->R.rax = open(f->R.rdi);
        break;
    case SYS_CLOSE:
        close(f->R.rdi);
        break;
    case SYS_CREATE:
        f->R.rax = create(f->R.rdi, f->R.rsi);
        break;
    case SYS_REMOVE:
        f->R.rax = remove(f->R.rdi);
        break;
    case SYS_TELL:
        f->R.rax = tell(f->R.rdi);
        break;
    case SYS_SEEK:
        seek(f->R.rdi, f->R.rsi);
        break;
    case SYS_FILESIZE:
        f->R.rax = filesize(f->R.rdi);
        break;
    case SYS_READ:
        f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
        break;
    case SYS_WRITE:
        f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
        break;
    default:
        exit(EXIT_FAILURE);
    }
}

static void check_valid_ptr(const void* u_addr)
{
    if (u_addr == NULL || !is_user_vaddr(u_addr) || !pml4_get_page(thread_current()->pml4, u_addr))
        exit(EXIT_FAILURE);
}

static void check_writeable_ptr(const void* u_addr)
{
    uint64_t* pte = pml4e_walk(thread_current()->pml4, u_addr, 0);

    if (pte == NULL || !is_writable(pte))
        exit(EXIT_FAILURE);
}

static void check_valid_buffer(const void* u_addr, size_t size, bool check_write)
{
    uint8_t* start_addr = (uint8_t*)u_addr;
    uint8_t* end_addr = start_addr + size - 1;
    if (end_addr < start_addr)
        exit(EXIT_FAILURE);

    uintptr_t current_page = (uintptr_t)pg_round_down(start_addr);
    uintptr_t end_page = (uintptr_t)pg_round_down(end_addr);
    for (uintptr_t page = current_page; page <= end_page; page += PGSIZE) {
        const uint8_t* check_addr = (page < (uintptr_t)start_addr) ? start_addr : (const uint8_t*)page;
        if (check_addr > end_addr)
            check_addr = end_addr;

        check_valid_ptr(check_addr);
        if (check_write)
            check_writeable_ptr(check_addr);
    }
}

static bool fd_low_order(const struct list_elem* a_, const struct list_elem* b_, void* aux UNUSED)
{
    const struct fd_* a = list_entry(a_, struct fd_, elem);
    const struct fd_* b = list_entry(b_, struct fd_, elem);

    return a->fd < b->fd;
}

static void exit(int status)
{
    thread_current()->exit_status = status;
    thread_exit();
}

static void halt(void)
{
    power_off();
}

static bool create(const char* file, int32_t initial_size)
{
    check_valid_ptr(file);

    lock_acquire(&syslock);
    bool is_created = filesys_create(file, initial_size);
    lock_release(&syslock);

    return is_created;
}

static bool remove(const char* file)
{
    check_valid_ptr(file);

    lock_acquire(&syslock);
    bool is_removed = filesys_remove(file);
    lock_release(&syslock);

    return is_removed;
}

static int open(const char* file)
{
    check_valid_ptr(file);

    struct thread* t = thread_current();

    lock_acquire(&syslock);
    struct file* f = filesys_open(file);
    lock_release(&syslock);

    if (f == NULL) 
        return EXIT_FAILURE;

    struct fd_* fd = malloc( sizeof(struct fd_));

    if (fd == NULL) {
        file_close(f);
        return EXIT_FAILURE;
    }

    int hubo = FD_MIN;
    list_sort(&t->fd_table, fd_low_order, NULL);

    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        struct fd_* cur = list_entry(e, struct fd_, elem);

        if (hubo == cur->fd)
            hubo++;
        else break;
    }

    fd->file = f;
    fd->fd = hubo;
    list_push_back(&t->fd_table, &fd->elem);
    return fd->fd;
}

static void close(int fd)
{
    if (fd == 0 || fd == 1)
        exit(EXIT_FAILURE);

    struct thread* t = thread_current();
    struct file* close_file = NULL;
    struct fd_* close_fd = NULL;

    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        struct fd_* cur = list_entry(e, struct fd_, elem);
        if (cur->fd == fd) {
            close_file = cur->file;
            close_fd = cur;
            break;
        }
    }

    if (close_file == NULL || close_fd == NULL)
        exit(EXIT_FAILURE);

    lock_acquire(&syslock);
    file_close(close_file);
    list_remove(&close_fd->elem);
    free(close_fd);
    lock_release(&syslock);
}

static unsigned tell(int fd)
{
    if (fd == 0 || fd == 1)
        return -1;

    uint64_t position = -1;
    struct thread* t = thread_current();
    struct fd_* cur;
    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        cur = list_entry(e, struct fd_, elem);
        if (cur->fd == fd) {
            lock_acquire(&syslock);
            position = file_tell(cur->file);
            lock_release(&syslock);
            return position;
        }
    }

    return position;
}

static void seek(int fd, unsigned position)
{
    if (fd == 0 || fd == 1)
        return -1;

    struct thread* t = thread_current();

    struct fd_* cur;
    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        cur = list_entry(e, struct fd_, elem);
        if (cur->fd == fd) {
            lock_acquire(&syslock);
            file_seek(cur->file, position);
            lock_release(&syslock);
        }
    }
}

static int filesize(int fd)
{
    if (fd == 0 || fd == 1)
        return -1;

    int file_size = -1;
    struct thread* t = thread_current();

    struct fd_* cur;
    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        cur = list_entry(e, struct fd_, elem);
        if (cur->fd == fd) {
            lock_acquire(&syslock);
            file_size = file_length(cur->file);
            lock_release(&syslock);
            return file_size;
        }
    }
    return file_size;
}

static int read(int fd, void* buffer, unsigned size)
{
    check_valid_ptr(buffer);

    char* buf = buffer;

    if (size == 0)
        return 0;

    if (fd == 0) {
        for (int i = 0; i < size; i++) {
            char temp = input_getc();
            buf[i] = temp;
        }
        return size;
    }

    int read_byte = -1;

    struct thread* t = thread_current();
    // list_sort(&t->fd_table, fd_low_order, NULL);
    struct fd_* cur;

    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        cur = list_entry(e, struct fd_, elem);
        if (cur->fd == fd) {
            lock_acquire(&syslock);
            read_byte = file_read(cur->file, buffer, size);
            lock_release(&syslock);
            return read_byte;
        }
    }

    return read_byte;
}

static int write(int fd, void* buffer, unsigned size)
{
    check_valid_ptr(buffer);

    if (size == 0)
        return size;

    if (fd == 1) {
        putbuf(buffer, size);
        return size;
    }

    int write_byte = 0;
    struct thread* t = thread_current();
    if (t == NULL)
        return -1;
    list_sort(&t->fd_table, fd_low_order, NULL);

    struct fd_* cur;
    for (struct list_elem* e = list_begin(&t->fd_table); e != list_end(&t->fd_table); e = list_next(e)) {

        cur = list_entry(e, struct fd_, elem);
        if (cur->fd == fd) {
            lock_acquire(&syslock);
            write_byte = file_write(cur->file, buffer, size);
            lock_release(&syslock);
            return write_byte;
        }
    }
}

static int exec(const char* cmd_line)
{
    
}