/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include <hash.h>

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	supplemental_page_table_init(&thread_current()->spt);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
uint64_t page_hash (const struct hash_elem *e, void *aux);
bool va_less (const struct hash_elem *a, const struct hash_elem *b, void *aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	/* TODO: Create the page, fetch the initializer according to the VM type, */
	/* TODO: and then create "uninit" page struct by calling uninit_new. */
	if (spt_find_page (spt, upage) == NULL) {

		struct page *p = malloc(sizeof(struct page));

		switch (VM_TYPE(type)) {
			case VM_ANON:
				uninit_new(p, upage, init, type, aux, anon_initializer);
				break;
			case VM_FILE:
				uninit_new(p, upage, init, type, aux, file_backed_initializer);
				break;
			default:
				break;
		}
		
		/* TODO: You should modify the field after calling the uninit_new. */
		p->writable = writable;

		if (!spt_insert_page(&spt->pages, p)) 
			goto err;

		return true;
	}
err:
	return false;
}


/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page *page;

	struct page dummy;
	dummy.va = va;

	struct hash_elem *dummy_h = hash_find(&spt->pages, &dummy.hash_elem);
	if (dummy_h == NULL)
		return NULL;

	page = hash_entry(dummy_h, struct page, hash_elem);
	if (page == NULL)
		return NULL;
	
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {

	struct hash_elem *e = hash_insert(&spt->pages, &page->hash_elem);
	//hash_insert() 은 hash 안에 동일한 elem이 없을 시에 추가한 후 NULL 값을 반환한다.
	if (e == NULL)
		return true;

	return false;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash_elem *e = hash_delete(&spt->pages, &page->hash_elem);

	if (e != NULL) {
		vm_dealloc_page (page);
	}

	return;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc(sizeof(struct frame));
	if (frame == NULL)
		return NULL;

	void *kva = palloc_get_page(PAL_USER);
	if (kva == NULL) {
		free(frame);
		PANIC("todo"); // todo: eviction algorithm
	}

	frame->kva = kva;
	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
    
    if (is_kernel_vaddr(addr) && user) return false;

    // 주소 정렬 후 페이지 검색
    void *page_start = pg_round_down(addr);
    struct page *page = spt_find_page(spt, page_start);

    // 페이지가 없는 경우 (NULL) -> 스택 증가인지 확인 */
    if (page == NULL) {
        return false; // 임시
    }

    // write on r/o
    if (!not_present && write) {
        return false; 
    }
    
	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page;
	/* TODO: Fill this function */
	void *page_start = pg_round_down(va);
	page = spt_find_page(&thread_current()->spt, page_start);
	if (page == NULL) {
		return false;
	}

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	bool succ = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	if (!succ) {
		free(frame);
		return false;
	}

	return swap_in (page, frame->kva);
}

uint64_t page_hash (const struct hash_elem *e, void *aux) {
	struct page* p = hash_entry(e, struct page, hash_elem);
	uint64_t hash = hash_bytes(&p->va,sizeof(void*));
	return hash;
}

bool va_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
	struct page* p_a = hash_entry(a, struct page, hash_elem);
	struct page* p_b = hash_entry(b, struct page, hash_elem);
	return p_a->va < p_b->va;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->pages, page_hash, va_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	// Iterate through each page in the src's supplemental page table 
	struct hash_iterator i;
	hash_first (&i, &src->pages);
   	while (hash_next (&i))
	{
		struct page *parent_page = hash_entry (hash_cur (&i), struct page, hash_elem);
		enum vm_type type = parent_page->operations->type;
		void *child_page = parent_page->va;
		bool writable = parent_page->writable;

		// only set initailizer for uninit pages for lazy loading
		if (type == VM_UNINIT) {
			vm_initializer *init = parent_page->uninit.init;
			enum vm_type ref_type = parent_page->uninit.type;
			struct lazy_aux *parent_aux = parent_page->uninit.aux;
			
			// need to copy aux of parent page
			// but aux only exists if type is VM_FILE
			if (ref_type & VM_FILE) {
				
				struct lazy_aux *child_aux = malloc(sizeof(struct lazy_aux));
				if (child_aux == NULL)
					return false;

				memcpy(child_aux, parent_aux, sizeof(struct lazy_aux));

				child_aux->file = file_reopen(parent_aux->file);
				if (child_aux->file == NULL) {
					free(child_aux);
					return false;
				}
				// how do you deep copy this?
				if (!vm_alloc_page_with_initializer(ref_type, child_page, writable, init, child_aux)) {
					free(child_aux);
					return false;
				}
			}

			else {
				if (!vm_alloc_page_with_initializer(ref_type, child_page, writable, init, parent_aux))
					return false;
			}
		} 

		else { // alloc and claim page for anon & file pages
			if (!vm_alloc_page(type, child_page, writable))
				return false;
			if (!vm_claim_page(child_page))
				return false;
			
			struct page *dst_page = spt_find_page(&thread_current()->spt, child_page);
			if (dst_page) {
				memcpy(dst_page->frame->kva, parent_page->frame->kva, PGSIZE);
			}
		}

   	}
	return true;
}

void spt_destroy_func(struct hash_elem *e) {
	struct page *p = hash_entry(e, struct page, hash_elem);
	vm_dealloc_page(p);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and */
	/* TODO: writeback all the modified contents to the storage. */
	hash_destroy(&spt->pages, spt_destroy_func);
}
