/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/syscall.h" // filesys_lock을 알기 위해
#include "threads/synch.h"    // lock_acquire/release를 알기 위해
#include "filesys/file.h"     // file_close를 알기 위해
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
static uint64_t page_hash (const struct hash_elem *e, void *aux);
static bool va_less (const struct hash_elem *a, const struct hash_elem *b, void *aux);
// for spt copy
static struct lazy_aux *copy_lazy_aux(struct lazy_aux *src_aux);
static bool copy_uninit_page(struct page *parent_page, void *upage, bool writable);
static bool copy_claimed_page(struct supplemental_page_table *dst, struct page *parent_page, void *upage, bool writable);
// for fault handling - stack growth check
static bool is_stack_growth (void *addr, uintptr_t rsp);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

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

		if (!spt_insert_page(spt, p)) {
			free(p);
			goto err;
		}
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

	void *kva = palloc_get_page(PAL_USER | PAL_ZERO);
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
static bool
vm_stack_growth (void *addr) {
	if (vm_alloc_page((VM_ANON | VM_MARKER_0), addr, true))
		return vm_claim_page(addr);
	return false;
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
    
	// There are three cases of bogus page fault: 
	// (1) lazy-loaded (2) swapped-out page (3)write-protected page
	// If it is a page fault for lazy loading, 
	// the kernel calls one of the initializers you previously set in vm_alloc_page_with_initializer to lazy load the segment. 

    // 주소 정렬 후 페이지 검색
    void *page_start = pg_round_down(addr);
    struct page *page = spt_find_page(spt, page_start);
	uintptr_t rsp = user ? f->rsp : thread_current()->rsp;

	if (is_kernel_vaddr(addr) && user) return false; // page fault handler에서 처리하긴 하는데 fast fail기능 및 함수 목적성에 부합

	if (page != NULL) { // 페이지 존재: LAZY LOADING || SWAP IN
		// write on r/o
	  	if (!page->writable && write) {
        	return false; 
    	}
		
		return vm_do_claim_page (page);	
	}

    // 페이지가 부재 -> 스택 증가인지 확인
	if (is_stack_growth(addr, rsp)) 
		return vm_stack_growth(page_start);	

	return false;
}

// 주소와 RSP를 받아 스택 증가가 가능한지 판단
static bool
is_stack_growth (void *addr, uintptr_t rsp) {

    bool valid_stack_range = 
        (uintptr_t)addr >= (uintptr_t)(USER_STACK - (1 << 20)) &&  // 스택 최대 크기 1MB 초과하지 않고
        (uintptr_t)addr < (uintptr_t)USER_STACK &&	// 스택주소 최댓값(시작점)보다 작고
		(uintptr_t)addr >= rsp - 8;	// PUSH 고려하였을 때 유효한 rsp인지

    return valid_stack_range;
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
/* hash_init 함수들어가는 hash 값 생성 함수 */
static uint64_t page_hash (const struct hash_elem *e, void *aux) {
	struct page* p = hash_entry(e, struct page, hash_elem);
	uint64_t hash = hash_bytes(&p->va,sizeof(void*));
	return hash;
}
/* hash_init 함수들어가는 bucket 별 리스트 삽입시 va값 순 정렬 함수 */
static bool va_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
	struct page* p_a = hash_entry(a, struct page, hash_elem);
	struct page* p_b = hash_entry(b, struct page, hash_elem);
	return p_a->va < p_b->va;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->pages, page_hash, va_less, NULL);
}

bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
        struct supplemental_page_table *src) {
    
    struct hash_iterator i;
    hash_first (&i, &src->pages);
    
    while (hash_next (&i)) {
        struct page *parent_page = hash_entry (hash_cur (&i), struct page, hash_elem);
        enum vm_type type = parent_page->operations->type;
        void *upage = parent_page->va;
        bool writable = parent_page->writable;

        if (type == VM_UNINIT) {
            /* Case 1: 아직 로딩되지 않은 페이지 (UNINIT) */
            if (!copy_uninit_page(parent_page, upage, writable))
                return false;
        } else {
            /* Case 2: 이미 메모리에 로딩된 페이지 (ANON, FILE) */
            if (!copy_claimed_page(dst, parent_page, upage, writable))
                return false;
        }
    }
    return true;
}

/* spt_copy helper(1): UNINIT 페이지 처리 
- VM_FILE인 경우 aux를 복사하고, 아니면 그대로 넘김. */
static bool 
copy_uninit_page(struct page *parent_page, void *upage, bool writable) {
    vm_initializer *init = parent_page->uninit.init;
    enum vm_type ref_type = parent_page->uninit.type;
    void *parent_aux = parent_page->uninit.aux;
    void *child_aux = parent_aux; // 기본값: 얕은 복사 (VM_ANON 등은 부모 aux 그대로 사용)

    /* 특수 케이스: VM_FILE은 깊은 복사로 덮어쓰기 */
    if (VM_TYPE(ref_type) == VM_FILE) {
        child_aux = copy_lazy_aux((struct lazy_aux *)parent_aux);
        
        /* Deep Copy 실패 시 (부모 aux는 있는데 자식 aux 할당 못함) */
        if (parent_aux != NULL && child_aux == NULL)
            return false;
    }

    /* 페이지 할당 요청 */
    if (!vm_alloc_page_with_initializer(ref_type, upage, writable, init, child_aux)) {
        /* ⚠️ 주의: 할당 실패 시 '내가 malloc한 경우'에만 free 해야 함! */
        /* VM_ANON이라서 parent_aux를 그대로 썼는데 free 하면 큰일 남 (Double Free 이슈) */
        if (VM_TYPE(ref_type) == VM_FILE && child_aux != NULL) {
            free(child_aux);
        }
        return false;
    }

    return true;
}
/* spt_copy helper(2): Claimed(Loaded) 페이지 처리 
- ANON/FILE 페이지의 구조체 생성, 메타데이터 복사, 프레임 복사를 담당합니다. */
static bool 
copy_claimed_page(struct supplemental_page_table *dst, struct page *parent_page, void *upage, bool writable) {
    enum vm_type type = parent_page->operations->type;

    if (!vm_alloc_page(type, upage, writable))
        return false;

    /* 자식 페이지 구조체 찾기 */
    struct page *dst_page = spt_find_page(dst, upage);
    if (dst_page == NULL)
        return false;

    /* 프레임 할당 (Claim) -> UNINIT 정보를 읽고 초기화 후 FILE 상태로 변신! */
    if (!vm_claim_page(upage))
        return false;

    /* VM_FILE 메타데이터 복사 */
    if (VM_TYPE(type) == VM_FILE) {
        struct file_page *parent_fp = &parent_page->file;
        struct file_page *child_fp = &dst_page->file;

        /* 기본 정보 복사 */
        memcpy(child_fp, parent_fp, sizeof(struct file_page));

        /* 파일 객체 Deep Copy */
        if (parent_fp->file != NULL) {
            lock_acquire(&filesys_lock);
            child_fp->file = file_reopen(parent_fp->file);
            lock_release(&filesys_lock);            
            
            if (child_fp->file == NULL) 
                return false;
        } 
        else {
            return false; 
        }
    }

    /* 물리 메모리 내용 복제 (Deep Copy) */
    memcpy(dst_page->frame->kva, parent_page->frame->kva, PGSIZE);

    return true;
}

/* spt_copy helper (3) ft.copy_uninit helper
==> Lazy Aux 깊은 복사 (Deep Copy) */
static struct lazy_aux *
copy_lazy_aux(struct lazy_aux *src_aux) {
    if (src_aux == NULL)
        return NULL;

    struct lazy_aux *dst_aux = malloc(sizeof(struct lazy_aux));
    if (dst_aux == NULL)
        return NULL;

    memcpy(dst_aux, src_aux, sizeof(struct lazy_aux));

    /* 파일 객체 Deep Copy (핵심) */
    lock_acquire(&filesys_lock);
    if (src_aux->file) {
        dst_aux->file = file_reopen(src_aux->file);
    } else {
        dst_aux->file = NULL;
    }
    lock_release(&filesys_lock);

    /* 파일 복제 실패 시 정리 */
    if (src_aux->file != NULL && dst_aux->file == NULL) {
        free(dst_aux);
        return NULL;
    }

    return dst_aux;
}

static void spt_destroy_func(struct hash_elem *e, void *aux UNUSED) {
	struct page *p = hash_entry(e, struct page, hash_elem);
	vm_dealloc_page(p);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and */
	/* TODO: writeback all the modified contents to the storage. */
	if (spt->pages.buckets != NULL) {
        hash_destroy (&spt->pages, spt_destroy_func);
    }
}