#include <stdio.h>
#include <syscall-nr.h>
#include "filesys/filesys.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/loader.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "vm/vm.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame*);

static void validate_ptr(void* ptr);
static void check_valid_fd(int fd);
static void validate_buffer(void* buffer, unsigned size, bool to_write);
static void try_prefault(char* addr, bool to_write);
static int allocate_fd(struct thread* t);
static bool lesser_fd(struct list_elem* a, struct list_elem* b, void* aux);
static struct file_descriptor* find_fd(struct thread* t, int fd);
static struct file* get_file_from_fd(int fd);
struct lock filesys_lock;

static void sys_halt(void);
static void sys_exit(int status);
static int sys_write(int fd, const void* buffer, unsigned size);
static bool sys_create(const char* file, unsigned initial_size);
static bool sys_remove(const char* file);
static int sys_open(const char* file_name);
static void sys_close(int fd);
static int sys_filesize(int fd);
static int sys_read(int fd, void* buffer, unsigned size);
static int sys_write(int fd, const void* buffer, unsigned size);
static void sys_seek(int fd, unsigned position);
static unsigned sys_tell(int fd);
static int sys_fork(const char* thread_name, struct intr_frame* _if);
static int sys_exec(const char* file);
static int sys_wait(tid_t pid);
static void* sys_mmap(void* addr,
                      size_t length,
                      int writable,
                      int fd,
                      off_t offset);
static void sys_munmap(void* addr);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 *  (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register  (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
    write_msr(MSR_STAR,
              ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    lock_init(&filesys_lock);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame* f UNUSED) {
    int syscall_number = f->R.rax;
    thread_current()->rsp = f->rsp;

    uint64_t arg1 = f->R.rdi;
    uint64_t arg2 = f->R.rsi;
    uint64_t arg3 = f->R.rdx;
    uint64_t arg4 = f->R.rcx;
    uint64_t arg5 = f->R.r8;

    switch (syscall_number) {
        case SYS_EXIT:
            sys_exit(arg1);
            break;
        case SYS_HALT:
            sys_halt();
            break;
        case SYS_CREATE:
            f->R.rax = sys_create(arg1, arg2);
            break;
        case SYS_REMOVE:
            f->R.rax = sys_remove(arg1);
            break;
        case SYS_OPEN:
            f->R.rax = sys_open(arg1);
            break;
        case SYS_CLOSE:
            sys_close(arg1);
            break;
        case SYS_READ:
            f->R.rax = sys_read(arg1, arg2, arg3);
            break;
        case SYS_WRITE:
            f->R.rax = sys_write(arg1, arg2, arg3);
            break;
        case SYS_SEEK:
            sys_seek(arg1, arg2);
            break;
        case SYS_TELL:
            f->R.rax = sys_tell(arg1);
            break;
        case SYS_FILESIZE:
            f->R.rax = sys_filesize(arg1);
            break;
        case SYS_FORK:
            f->R.rax = sys_fork(arg1, f);
            break;
        case SYS_EXEC:
            sys_exec(arg1);
            break;
        case SYS_WAIT:
            f->R.rax = sys_wait(arg1);
            break;
        case SYS_MMAP:
            f->R.rax = sys_mmap(arg1, arg2, arg3, arg4, arg5);
            break;
        case SYS_MUNMAP:
            sys_munmap(arg1);
            break;
        default:
            thread_exit();
    }
}
static void sys_halt(void) {
    power_off();
}

static void sys_exit(int status) {
    thread_current()->exit_status = status;
    thread_exit();
}

static bool sys_create(const char* file, unsigned initial_size) {
    validate_string(file);

    lock_acquire(&filesys_lock);
    bool success = filesys_create(file, initial_size);
    lock_release(&filesys_lock);

    return success;
}
static bool sys_remove(const char* file) {
    validate_ptr(file);

    lock_acquire(&filesys_lock);
    bool success = filesys_remove(file);
    lock_release(&filesys_lock);

    return success;
}

static int sys_open(const char* file_name) {
    validate_string(file_name);

    lock_acquire(&filesys_lock);

    struct file* file = filesys_open(file_name);
    if (file == NULL) {
        lock_release(&filesys_lock);
        return -1;
    }
    lock_release(&filesys_lock);

    struct thread* curr = thread_current();

    struct file_descriptor* fd = malloc(sizeof(struct file_descriptor));
    fd->fd_val = allocate_fd(curr);
    fd->fd_file = file;
    list_push_back(&curr->fd_table, &fd->fd_elem);

    return fd->fd_val;
}

static void sys_close(int fd) {
    check_valid_fd(fd);

    struct thread* cur = thread_current();

    lock_acquire(&filesys_lock);

    struct file_descriptor* fd_struct = find_fd(cur, fd);
    if (fd_struct == NULL) {
        lock_release(&filesys_lock);
        return;
    }
    struct file* file = fd_struct->fd_file;
    if (file == NULL) {
        lock_release(&filesys_lock);
        return;
    }
    list_remove(&fd_struct->fd_elem);

    file_close(file);
    lock_release(&filesys_lock);

    free(fd_struct);
}

static int sys_filesize(int fd) {
    check_valid_fd(fd);
    struct thread* curr = thread_current();

    struct file* file = get_file_from_fd(fd);
    if (file == NULL)
        return NULL;

    lock_acquire(&filesys_lock);
    int f_size = file_length(file);
    lock_release(&filesys_lock);

    return f_size;
}

static int sys_read(int fd, void* buffer, unsigned size) {
    validate_buffer(buffer, size, true);

    if (fd == 0) {
        input_getc();
        return;
    }

    check_valid_fd(fd);

    struct file* file = get_file_from_fd(fd);
    if (file == NULL)
        return -1;

    lock_acquire(&filesys_lock);
    int bytes_read = file_read(file, buffer, size);
    lock_release(&filesys_lock);

    if (bytes_read < size)
        return EOF;

    return bytes_read;
}

static int sys_write(int fd, const void* buffer, unsigned size) {
    validate_buffer(buffer, size, false);

    if (fd == 1) {
        putbuf(buffer, size);
        return size;
    }

    check_valid_fd(fd);

    struct file* file = get_file_from_fd(fd);
    if (file == NULL)
        return -1;

    lock_acquire(&filesys_lock);
    int bytes_written = file_write(file, buffer, size);
    lock_release(&filesys_lock);

    return bytes_written;
}

static void sys_seek(int fd, unsigned position) {
    check_valid_fd(fd);

    struct file* file = get_file_from_fd(fd);
    if (file == NULL)
        return -1;

    lock_acquire(&filesys_lock);
    file_seek(file, position);
    lock_release(&filesys_lock);
}

static unsigned sys_tell(int fd) {
    check_valid_fd(fd);

    struct file* file = get_file_from_fd(fd);
    if (file == NULL)
        return -1;

    lock_acquire(&filesys_lock);
    unsigned start = file_tell(file);
    lock_release(&filesys_lock);

    return start;
}

static tid_t sys_fork(const char* thread_name, struct intr_frame* f) {
    validate_ptr(thread_name);

    return process_fork(thread_name, f);
}

static int sys_exec(const char* file) {
    validate_string(file);
    char* f_cpy = palloc_get_page(0);
    if (f_cpy == NULL)
        sys_exit(-1);

    strlcpy(f_cpy, file, PGSIZE);

    if (process_exec(f_cpy) == -1)
        sys_exit(-1);

    NOT_REACHED();  // exec 성공하면 원래 프로세스는 돌아오지 않음
}

static int sys_wait(tid_t pid) {
    int status = process_wait(pid);
    return status;
}

static void* sys_mmap(void* addr,
                      size_t length,
                      int writable,
                      int fd,
                      off_t offset) {
    validate_ptr(addr);
    check_valid_fd(fd);

    struct file* file = get_file_from_fd(fd);
    if (file == NULL)
        return NULL;

    lock_acquire(&filesys_lock);

    lock_release(&filesys_lock);
}

static void sys_munmap(void* addr) {}

/* ########### HELPER FUNCTIONS ############## */
static void validate_ptr(void* ptr) {
    if (ptr == NULL || is_kernel_vaddr(ptr))  // if invalid ptr
        sys_exit(-1);
}

static void check_valid_fd(int fd) {
    if (fd < MIN_FD || fd > MAX_FD)
        sys_exit(-1);
}

/* helper: 주소를 찔러서 (Touch) Page Fault를 유도 */
static void try_prefault(char* addr, bool to_write) {
    validate_ptr(addr);

    struct supplemental_page_table* spt = &thread_current()->spt;
    struct page* page = spt_find_page(spt, pg_round_down(addr));

    if (page != NULL && to_write && !page->writable) {
        sys_exit(-1);
    }

    /* 실제로 건드리기  (Pre-faulting) */
    if (to_write) {
        /* 쓰기 시도: 읽어서 다시 씀  (Dirty bit, Writable 체크) */
        volatile char dummy = *addr;
        *addr = dummy;  // 읽은 값 그대로 다시 쓰기  (훼손 ㄴ)
    } else {
        /* 읽기 시도: 그냥 읽어봄  (Present bit 체크) */
        volatile char dummy = *addr;
        (void)dummy;
    }
}

static void validate_buffer(void* buffer, unsigned size, bool to_write) {
    char* start = (char*)buffer;
    char* end = start + size;

    // 시작 주소부터 페이지 단위로 점프하며 찌르기
    for (char* addr = start; addr < end; addr += PGSIZE) {
        try_prefault(addr, to_write);
    }
    /* 2. 마지막 주소도 찌르기  (중요: 페이지 경계에 걸쳐 있을 때 필수) */
    /* 예: buffer가 0x1000에서 시작해 4097바이트라면,
       위 루프는 0x1000만 검사하므로 0x2000 (마지막 1바이트)도 검사해야 함 */
    if (size > 0) {
        try_prefault(end - 1, to_write);
    }
}

void validate_string(const char* str) {
    validate_ptr(str);
    while (*str != '\0') {
        validate_ptr(str);  // str이 valid page인지 확인
        str++;
    }
    // 마지막 '\0'도 검사
    validate_ptr(str);
}

// returns available fd
static int allocate_fd(struct thread* t) {
    struct list* files = &t->fd_table;
    int fd = MIN_FD;

    if (!list_empty(files)) {
        list_sort(files, lesser_fd, NULL);  // fd값 낮은 순

        struct list_elem* e;
        for (e = list_begin(files); e != list_end(files);) {
            struct list_elem* next = list_next(e);
            struct file_descriptor* curr =
                list_entry(e, struct file_descriptor, fd_elem);

            if (curr->fd_val == fd) {
                fd += 1;
                e = next;
            } else
                break;  // curr->fd_val > fd ==> 해당 fd 값 가능
        }
    }
    return fd;
}

static bool lesser_fd(struct list_elem* a, struct list_elem* b, void* aux) {
    struct file_descriptor* fd_a =
        list_entry(a, struct file_descriptor, fd_elem);
    struct file_descriptor* fd_b =
        list_entry(b, struct file_descriptor, fd_elem);
    return fd_a->fd_val < fd_b->fd_val;
}

static struct file_descriptor* find_fd(struct thread* t, int fd) {
    struct list* files = &t->fd_table;

    struct list_elem* e;
    for (e = list_begin(files); e != list_end(files);) {
        struct list_elem* next = list_next(e);
        struct file_descriptor* curr =
            list_entry(e, struct file_descriptor, fd_elem);
        if (curr->fd_val == fd)
            return curr;
        e = next;
    }

    return NULL;
}

/* fd를 통해 file 객체를 찾아 반환하는 헬퍼 함수 */
static struct file* get_file_from_fd(int fd) {
    /* 1. fd 범위 체크  (이미 check_valid_fd가 있다면 생략 가능하지만 안전을
     * 위해)
     */
    if (fd < 0 || fd >= MAX_FD)
        return NULL;

    /* 2. fd_table에서 entry 찾기 */
    struct file_descriptor* fd_struct = find_fd(thread_current(), fd);

    if (fd_struct == NULL)
        return NULL;

    /* 3. file 객체 반환  (NULL일 수도 있음) */
    return fd_struct->fd_file;
}