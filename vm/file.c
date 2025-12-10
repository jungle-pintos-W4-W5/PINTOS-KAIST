/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/syscall.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
	return;
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *fp= &page->file;
	if (fp->file != NULL)
		file_close(fp->file);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	// 여기로 넘기기 전에 조건들 이미 확인함
	void *base_addr = addr;
	uint32_t read_bytes = length;
	int pg_count = 0;

	while (read_bytes > 0) {

		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct lazy_aux *aux= malloc(sizeof(struct lazy_aux));
		if (aux == NULL)
			goto error;

		aux->file = file_reopen(file);
		aux->ofs = offset;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer (VM_FILE, addr,
					writable, lazy_load_segment, aux)) {
			file_close(aux->file);
			free(aux);
			goto error;
		}
		/* Advance. */
		read_bytes -= page_read_bytes;
		offset += page_read_bytes;
		addr += PGSIZE;
		pg_count += 1;
	}

	return base_addr;

error: // dealloc opened pages until failure
	addr = base_addr;
	while (pg_count-- > 0) {
		struct page *p = spt_find_page(&thread_current()->spt,addr);
		if (p != NULL)
			vm_dealloc_page(p);
		addr += PGSIZE;
	}

	return NULL;
}


/* Do the munmap */
void
do_munmap (void *addr) {

	struct supplemental_page_table *spt = &thread_current()->spt;
	uint64_t *pml4 = thread_current()->pml4;
	struct page *p = spt_find_page(spt, addr);
	if (p == NULL || VM_TYPE(p->operations->type)!= VM_FILE)
		return;

	struct file *f = p->file.file;
	struct file *cur_f = f;

	for (addr; cur_f == f ; addr += PGSIZE) {
		// 다음 페이지
		struct page *cur_p = spt_find_page(spt,addr);
		if (cur_p == NULL || VM_TYPE(cur_p->operations->type) != VM_FILE)
			return;

		// 파일 맞는지 확인
		cur_f = cur_p->file.file;
		if (cur_f != f)
			return;

		// 정리하기 전 더러운지 확인
		if (pml4_is_dirty (pml4, addr)) {
			int ofs =  cur_p->file.ofs;
			int read_bytes = cur_p->file.read_bytes;
			file_write_at(cur_f , addr, read_bytes, ofs);
		}

		// 정리
		pml4_clear_page(pml4, addr);
		vm_dealloc_page(cur_p);
	}
}