/* linanqinqin */
/*
 * lame_sched.c - LAME bundle scheduling support
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <x86intrin.h>

#include <base/log.h>
#include <runtime/smalloc.h>

#include "defs.h"

DEFINE_PERTHREAD(uint64_t, lame_scratch);
DEFINE_PERTHREAD(uint8_t, in_lame) = 0;
#ifdef CONFIG_LAME_TSC
DEFINE_PERTHREAD(struct thread_tf, lame_tf);
#endif

/* External configuration variable */
extern unsigned int cfg_lame_bundle_size;
extern unsigned int cfg_emulation_lame_stall_cycles;

/**
 * lame_bundle_init - initializes a LAME bundle for a kthread
 * @k: the kthread to initialize the bundle for
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
void lame_bundle_init(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;

	/* Initialize the bundle structure */
	memset(bundle->uthreads, 0, sizeof(bundle->uthreads));
	bundle->size = cfg_lame_bundle_size;
	bundle->active = 0;
	bundle->used = 0;
	bundle->total_cycles = 0;
	bundle->total_lames = 0;
	bundle->total_xsave_lames = 0;
	bundle->total_xsave_cycles = 0;
	bundle->total_early_lames = 0;
	bundle->total_stall_lames = 0;
	bundle->total_skip_lames = 0;
	bundle->total_stall_cycles = 0;
	bundle->enabled = false; /* Start disabled */

}

/**
 * lame_bundle_cleanup - cleans up a LAME bundle
 * @k: the kthread whose bundle to clean up
 */
void lame_bundle_cleanup(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;

	/* Reset the bundle structure */
	memset(bundle->uthreads, 0, sizeof(bundle->uthreads));
	bundle->size = 0;
	bundle->active = 0;
	bundle->used = 0;
	bundle->total_cycles = 0;
	bundle->total_lames = 0;
	bundle->total_xsave_lames = 0;
	bundle->total_xsave_cycles = 0;
	bundle->total_early_lames = 0;
	bundle->total_stall_lames = 0;
	bundle->total_skip_lames = 0;
	bundle->total_stall_cycles = 0;
	bundle->enabled = false; /* Disable when cleaning up */
}

/**
 * lame_bundle_add_uthread - adds a uthread to the bundle
 * @k: the kthread
 * @th: the uthread to add
 * @set_active: if true, set this uthread as the active one in the bundle
 *
 * Returns 0 if successful, or -ENOSPC if bundle is full, or -EINVAL if invalid.
 */
int lame_bundle_add_uthread(struct kthread *k, thread_t *th, bool set_active)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int i;
	int first_empty_slot = -1;

	/* Iterate through the bundle to check for duplicates and find first empty slot */
	for (i = 0; i < bundle->size; i++) {
		if (bundle->uthreads[i].present) {
			/* Check for duplicate */
			if (bundle->uthreads[i].uthread == th) {
					log_err("[LAME]: attempted to add duplicate uthread %p to bundle (kthread %d)",
				 th, myk_index());
				return 0; /* Return gracefully, no error */
			}
		} else {
			/* Record first empty slot */
			if (first_empty_slot == -1) {
				first_empty_slot = i;
			}
		}
	}

	/* Check if we found an empty slot */
	if (first_empty_slot == -1) {
		return -ENOSPC;
	}

	/*
	 * Threads without a private FS base use the runtime FS base of the
	 * kthread on which they run. Secondary bundle members bypass jmp_thread(),
	 * so establish their effective FS base before LAME can select them.
	 *
	 * Keep has_fsbase false: if this thread later migrates, the destination
	 * kthread must be allowed to replace this value with its runtime FS base.
	 */
	if (!th->has_fsbase)
		th->fsbase = perthread_read(runtime_fsbase);

	/* Add the uthread to the first empty slot */
	bundle->uthreads[first_empty_slot].uthread = th;
	bundle->uthreads[first_empty_slot].present = true;
	bundle->uthreads[first_empty_slot].cycles = 0;
	bundle->uthreads[first_empty_slot].lame_count = 0;
	bundle->used++;
	
	/* If this is the uthread that will run next, update the active index */
	if (set_active) {
		bundle->active = first_empty_slot;
	}
	
	return 0;
}

/**
 * lame_bundle_remove_uthread - removes a uthread from the bundle
 * @k: the kthread
 * @th: the uthread to remove
 *
 * Returns 0 if successful, or -ENOENT if uthread not found.
 */
int lame_bundle_remove_uthread(struct kthread *k, thread_t *th)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int i;

	/* Find the uthread in the bundle */
	for (i = 0; i < bundle->size; i++) {
		if (bundle->uthreads[i].present && bundle->uthreads[i].uthread == th) {

			bundle->uthreads[i].present = false;
			bundle->uthreads[i].uthread = NULL;
			bundle->used--;
			
			return 0;
		}
	}

	return -ENOENT;
}

/**
 * lame_bundle_remove_uthread_by_index - removes a uthread from the bundle by index
 * @k: the kthread
 * @index: the index of the uthread to remove
 *
 * Returns 0 if successful, or -EINVAL if index is out of bounds, or -ENOENT if uthread not present.
 */
int lame_bundle_remove_uthread_by_index(struct kthread *k, unsigned int index)
{
	struct lame_bundle *bundle = &k->lame_bundle;

	if (index >= bundle->size) {
		log_err("[LAME][kthread:%d][func:lame_bundle_remove_uthread_by_index] index %u out of bounds",
			myk_index(), index);
		return -EINVAL;
	}

	if (bundle->uthreads[index].present) {
		bundle->uthreads[index].present = false;
		bundle->uthreads[index].uthread = NULL;
		bundle->used--;
		return 0;
	}
	else {
		return -ENOENT;
	}
}

/**
 * lame_bundle_remove_uthread_at_active - removes the uthread at the active index
 * @k: the kthread
 *
 * Returns 0 if successful, or -ENOENT if uthread not present.
 */
int lame_bundle_remove_uthread_at_active(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int active = bundle->active;
	
	if (bundle->uthreads[active].present) {

		bundle->uthreads[active].present = false;
		bundle->uthreads[active].uthread = NULL;
		bundle->used--;
		return 0;
	}
	else {
		return -ENOENT;
	}
}

/**
 * lame_bundle_get_used_count - gets the number of uthreads currently in the bundle
 * @k: the kthread
 *
 * Returns the number of active uthreads.
 */
__always_inline __nofp unsigned int lame_bundle_get_used_count(struct kthread *k)
{
	 return k->lame_bundle.used;
}
 
/**
 * lame_sched_get_next_uthread - gets the next uthread to run in round-robin fashion
 * @k: the kthread
 *
 * Returns the next uthread to run, or NULL if none available.
 * The active field in the bundle represents the currently running uthread.
 */
__always_inline __nofp static thread_t *lame_sched_get_next_uthread(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int start_idx, i;

	start_idx = bundle->active;
	
	/* Search for the next present uthread starting from current index */
	for (i = 1; i <= bundle->size; i++) {
		unsigned int idx = (start_idx + i) % bundle->size;
		if (bundle->uthreads[idx].present) {
			bundle->active = idx;
			return bundle->uthreads[idx].uthread;
		}
	}

	return NULL;
}

/**
 * lame_sched_get_next_idx_uthread - fast path to get the next uthread to run
 * @k: the kthread
 *
 * this function assumes that a bundle is filled to the first bundle.size slots 
 */
__always_inline __nofp static thread_t *lame_sched_get_next_idx_uthread(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int next_idx = bundle->active + 1;
	if (next_idx >= bundle->used) {
		next_idx = 0;
	}
	bundle->active = next_idx;
	return bundle->uthreads[next_idx].uthread;
}

/**
 * lame_sched_get_current_uthread - gets the currently active uthread
 * @k: the kthread
 *
 * Returns the currently active uthread, or NULL if none.
 * The active field in the bundle represents the currently running uthread.
 */
__always_inline __nofp static thread_t *lame_sched_get_current_uthread(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;

	/* The current uthread is at the active index */
	if (bundle->uthreads[bundle->active].present)
		return bundle->uthreads[bundle->active].uthread;

	return NULL;
}

/**
 * lame_sched_get_current_uthread_nocheck - gets the currently active uthread without checking if it is present
 * @k: the kthread
 *
 * Returns the currently active uthread.
 */
 __always_inline __nofp static thread_t *lame_sched_get_current_uthread_nocheck(struct kthread *k)
 {
	struct lame_bundle *bundle = &k->lame_bundle;
	return bundle->uthreads[bundle->active].uthread;
 }

/**
 * lame_sched_is_enabled - checks if bundle scheduling is enabled
 * @k: the kthread
 *
 * Returns true if bundle scheduling is dynamically enabled,
 * false otherwise.
 */
__always_inline __nofp bool lame_sched_is_enabled(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	return bundle->enabled;
}

/**
 * lame_sched_enable - dynamically enables bundle scheduling
 * @k: the kthread
 *
 * This function enables bundle scheduling at runtime. It should be called
 * when entering safe sections where bundle scheduling is allowed.
 * Note: Bundle scheduling must be statically enabled (size > 1) for this
 * to have any effect.
 */
__always_inline __nofp void lame_sched_enable(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	
	bundle->enabled = true;
}

/**
 * lame_sched_disable - dynamically disables bundle scheduling
 * @k: the kthread
 *
 * This function disables bundle scheduling at runtime. It should be called
 * when entering critical sections where bundle scheduling should be avoided
 * (e.g., during yield operations, scheduler critical sections).
 */
__always_inline __nofp void lame_sched_disable(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	
	bundle->enabled = false;
}

/**
 * lame_sched_is_statically_enabled - checks if bundle scheduling is statically enabled
 * @k: the kthread
 *
 * Returns true if bundle scheduling is statically enabled (bundle size > 1),
 * false otherwise. This is a configuration check, not a runtime state.
 */
__always_inline __nofp bool lame_sched_is_statically_enabled(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	return bundle->size > 1;
}

/**
 * lame_sched_is_dynamically_enabled - checks if bundle scheduling is dynamically enabled
 * @k: the kthread
 *
 * Returns true if bundle scheduling is dynamically enabled (enabled flag is true),
 * false otherwise. This should be checked after confirming static enablement
 * with lame_sched_is_statically_enabled().
 */
__always_inline __nofp bool lame_sched_is_dynamically_enabled(struct kthread *k)
{
	return k->lame_bundle.enabled;
}

/**
 * lame_bundle_print - prints the bundle array in a neat format
 * @k: the kthread whose bundle to print
 *
 * This function prints the bundle structure in a readable format:
 * - First line shows the bundle metadata fields
 * - Subsequent lines show each uthread wrapper entry
 */
void lame_bundle_print(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int i;
	char log_buf[512];
	int offset;

	/* linanqinqin */
	offset = snprintf(log_buf, sizeof(log_buf), 
			"[LAME][BUNDLE][kthread:%d][size:%u][used:%u][active:%u][enabled:%d][bundle:", 
			myk_index(), bundle->size, bundle->used, bundle->active, bundle->enabled);

	/* Print each uthread wrapper entry in format: <uthread0><uthread1>...<uthreadn-1> */
	for (i = 0; i < bundle->size && offset < (int)sizeof(log_buf) - 1; i++) {
		struct lame_uthread_wrapper *wrapper = &bundle->uthreads[i];
		offset += snprintf(log_buf + offset, sizeof(log_buf) - offset, 
				  "<%p>", wrapper->uthread);
	}

	/* Close the bundle bracket */
	offset += snprintf(log_buf + offset, sizeof(log_buf) - offset, "]");
	/* end */

	log_info("%s", log_buf); 
}

/**
 * lame_bundle_set_ready_false_all - sets thread_ready to false for all uthreads in the bundle
 * @k: the kthread
 *
 * This function sets thread_ready = false for all uthreads in the bundle.
 * This is called when uthreads are added to the bundle to maintain the illusion
 * that they are "running" from Caladan's perspective.
 */
__always_inline void lame_bundle_set_ready_false_all(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int i;

	for (i = 0; i < bundle->size; i++) {
		if (bundle->uthreads[i].present) {
			bundle->uthreads[i].uthread->thread_ready = false;
		}
	}
}

/**
 * lame_bundle_set_running_true_all - sets thread_running to true for all uthreads in the bundle
 * @k: the kthread
 *
 * This function sets thread_running = true for all uthreads in the bundle.
 * This is called when uthreads are added to the bundle to maintain the illusion
 * that they are "running" from Caladan's perspective.
 */
__always_inline void lame_bundle_set_running_true_all(struct kthread *k)
{
	struct lame_bundle *bundle = &k->lame_bundle;
	unsigned int i;

	for (i = 0; i < bundle->size; i++) {
		if (bundle->uthreads[i].present) {
			bundle->uthreads[i].uthread->thread_running = true;
		}
	}
}

__always_inline static void __lame_bundle_to_rq(struct kthread *k) 
{
	struct lame_bundle *bundle = &k->lame_bundle;
	uint64_t now_tsc = rdtsc();

	/* Iterate through all bundle slots */
	for (unsigned int i = 0; i < bundle->size; i++) {
		if (bundle->uthreads[i].present) {
			thread_t *th = bundle->uthreads[i].uthread;
			uint32_t rq_tail;
			
			/* Mark uthread as ready */
			th->thread_ready = true;
			th->thread_running = false;
			th->ready_tsc = now_tsc;
			
			/* Add uthread back to runqueue (similar to thread_ready but without accounting) */
			rq_tail = load_acquire(&k->rq_tail);
			if (unlikely(k->rq_head - rq_tail >= RUNTIME_RQ_SIZE ||
			             !list_empty_volatile(&k->rq_overflow))) {
				/* Runqueue is full, add to overflow list */
				assert(k->rq_head - rq_tail <= RUNTIME_RQ_SIZE);
				list_add_tail(&k->rq_overflow, &th->link);
				drain_overflow(k);
				ACCESS_ONCE(k->q_ptrs->rq_head)++;
			} else {
				/* Add to main runqueue */
				k->rq[k->rq_head % RUNTIME_RQ_SIZE] = th;
				store_release(&k->rq_head, k->rq_head + 1);
				if (k->rq_head - load_acquire(&k->rq_tail) == 1)
					ACCESS_ONCE(k->q_ptrs->oldest_tsc) = th->ready_tsc;
				ACCESS_ONCE(k->q_ptrs->rq_head)++;
			}
			
			/* Clear the bundle slot */
			bundle->uthreads[i].present = false;
			bundle->uthreads[i].uthread = NULL;
			bundle->uthreads[i].cycles = 0;
			bundle->uthreads[i].lame_count = 0;
		}
	}
}

/**
 * lame_sched_bundle_dismantle - dismantles the bundle and returns all uthreads to runqueue
 * @k: the kthread
 *
 * This function is called when a uthread is descheduled to dismantle the entire bundle.
 * It removes all uthreads from the bundle and adds them back to Caladan's runqueue.
 * This ensures bundle lifecycle is tied to Caladan's scheduling lifecycle.
 *
 * The function does not perform accounting or statistics as those are handled by
 * Caladan's regular descheduling procedure.
 */
void lame_sched_bundle_dismantle(struct kthread *k)
{
	if (k->lame_bundle.used >= 1) {
		spin_lock(&k->lock);
		__lame_bundle_to_rq(k);
		spin_unlock(&k->lock);
	}

	/* Reset bundle state */
	k->lame_bundle.used = 0;
	k->lame_bundle.active = 0;
}

/**
 * lame_sched_bundle_dismantle - dismantles the bundle and returns all uthreads to runqueue
 * @k: the kthread
 *
 * This function is identical to lame_sched_bundle_dismantle, but assumes the lock is already held.
 */
void lame_sched_bundle_dismantle_nolock(struct kthread *k)
{
	assert_spin_lock_held(&k->lock);

	if (k->lame_bundle.used >= 1) {
		__lame_bundle_to_rq(k);
	}

	/* Reset bundle state */
	k->lame_bundle.used = 0;
	k->lame_bundle.active = 0;
}

#ifdef CONFIG_LAME_XSAVEOPT
/* 
 * xsave buffer management
 */
unsigned char* lame_xsave_buf_alloc(void) 
{
	BUG_ON(xsave_max_size == 0);

	unsigned char *xs_buf_orig = (unsigned char *)__szalloc(xsave_max_size + 64 + sizeof(void *));
	unsigned char *xs_buf;
	uint64_t active_xstates;

	if (unlikely(!xs_buf_orig)) {
		return NULL;
	}

	/* Align to 64 bytes, leaving space for storing the original pointer */
	xs_buf = (unsigned char *)align_up((uintptr_t)(xs_buf_orig + sizeof(void *)), 64);
	/* Store original pointer right before aligned region for cleanup */
	*(void **)(xs_buf - sizeof(void *)) = xs_buf_orig;

	/* initialize with XSAVE */
	active_xstates = __builtin_ia32_xgetbv(0);
	__builtin_ia32_xsave64(xs_buf, active_xstates);

	return xs_buf;
}

void lame_xsave_buf_free(thread_t *th)
{
	BUG_ON(!th->tf.xsave_area);
	void *xs_buf_orig = *(void **)(th->tf.xsave_area - sizeof(void *));
	sfree(xs_buf_orig);
	th->tf.xsave_area = NULL;
}
#endif

extern uint64_t text_section_start;
extern uint64_t text_section_end;
extern uint64_t gpr_bitmap_size;
extern unsigned char *gpr_bitmap;

#ifdef CONFIG_DEBUG
DEFINE_PERTHREAD(uint64_t, rips[10]);
DEFINE_PERTHREAD(int, decisions[10]); /* -1=not in bitmap, 0=not SIMD-free (needs xsave), 1=SIMD-free (no xsave)*/
DEFINE_PERTHREAD(int, rip_idx) = 0;
static __always_inline __nofp bool needs_xsave(uint64_t rip) 
{
	int idx = perthread_read(rip_idx);
	if (idx < 9) {
		perthread_store(rip_idx, idx + 1);
	}
	perthread_store(rips[idx], rip);
	if (rip < text_section_start || rip >= text_section_end) {
		/* rip is not in the bitmap range; xsave by default */
		perthread_store(decisions[idx], -1);
		return true;
	}
	uint64_t page_idx = (rip - text_section_start) >> LAME_BITMAP_PGSZ_FACTOR;
	perthread_store(decisions[idx], (gpr_bitmap[page_idx>>LAME_BITMAP_BYTE_SHIFT] & (1UL << (page_idx & LAME_BITMAP_BYTE_MASK))) == 0);
	return (gpr_bitmap[page_idx>>LAME_BITMAP_BYTE_SHIFT] & (1UL << (page_idx & LAME_BITMAP_BYTE_MASK))) == 0;
}
#else
static __always_inline __nofp bool needs_xsave(uint64_t rip) 
{
	if (rip < text_section_start || rip >= text_section_end) {
		/* rip is not in the bitmap range; xsave by default */
		return true;
	}
	uint64_t page_idx = (rip - text_section_start) >> LAME_BITMAP_PGSZ_FACTOR;
	return (gpr_bitmap[page_idx>>LAME_BITMAP_BYTE_SHIFT] & (1UL << (page_idx & LAME_BITMAP_BYTE_MASK))) == 0;
}
#endif

static __always_inline __nofp bool is_apptext(uint64_t rip) 
{
	return (rip >= text_section_start && rip < text_section_end);
}

/**
 * lame_handle - handles LAME exception and performs context switch
 * 
 * This function is called from the assembly __lame_entry after volatile
 * registers are saved. It performs all the LAME handling logic:
 * 1. Get current kthread
 * 2. Check if LAME scheduling is enabled
 * 3. Get current uthread's trapframe
 * 4. Get next uthread from bundle
 * 5. Call __jmp_thread_direct to perform context switch
 */
__attribute__((optimize("O3")))
__always_inline __nofp void lame_handle(uint64_t rip)
{
	struct kthread *k = perthread_read(mykthread);
	thread_t *cur_th, *next_th;

	/* no switching if: 
	 * - not in app text
	 * - only one uthread in the bundle */
	if (unlikely( (!is_apptext(rip)) || 
					(lame_bundle_get_used_count(k) <= 1) )) {
		preempt_enable();
		perthread_decr(in_lame);
		lame_stall();
		return;
	}

	/* Get current and next uthreads from bundle 
	 * not checking null because that would be a fatal bug anyway */
	cur_th = lame_sched_get_current_uthread_nocheck(k);
	next_th = lame_sched_get_next_idx_uthread(k);

	/* Update __self to point to the new uthread */
	perthread_store(__self, next_th);

// #ifdef CONFIG_LAME_TSC
// 	struct thread_tf *lame_tf = perthread_ptr(lame_tf);

// 	lame_tf->rdi = next_th->tf.rdi;
// 	lame_tf->rsi = next_th->tf.rsi;
// 	lame_tf->rdx = next_th->tf.rdx;
// 	lame_tf->rcx = next_th->tf.rcx;
// 	lame_tf->r8 = next_th->tf.r8;
// 	lame_tf->r9 = next_th->tf.r9;
// 	lame_tf->r10 = next_th->tf.r10;
// 	lame_tf->r11 = next_th->tf.r11;
// 	lame_tf->rbx = next_th->tf.rbx;
// 	lame_tf->rbp = next_th->tf.rbp;
// 	lame_tf->r12 = next_th->tf.r12;
// 	lame_tf->r13 = next_th->tf.r13;
// 	lame_tf->r14 = next_th->tf.r14;
// 	lame_tf->r15 = next_th->tf.r15;
// 	lame_tf->rax = next_th->tf.rax;
// #endif

#ifdef CONFIG_LAME_TSC
	uint64_t tsc_reg_start = __rdtsc();
#endif

	if (unlikely(needs_xsave(rip))) {
		unsigned char *xsave_buf;
		unsigned long active_xstates;
#ifdef CONFIG_LAME_TSC
		uint64_t tsc_start = __rdtsc();
#endif
		/* xsave */
#ifdef CONFIG_LAME_XSAVEOPT
		xsave_buf = cur_th->tf.xsave_area;
		active_xstates = __builtin_ia32_xgetbv(0); 	/* get active xstates */
		__builtin_ia32_xsaveopt64(xsave_buf, active_xstates); 	/* save state */
#else
		xsave_buf = alloca(xsave_max_size + 64); 	/* allocate buffer for xsave area on stack */
		xsave_buf = (unsigned char *)align_up((uintptr_t)xsave_buf, 64); 	/* align to 64 bytes */
		__builtin_memset(xsave_buf + 512, 0, 64); 	/* zero xsave header */
		active_xstates = __builtin_ia32_xgetbv(0); 	/* get active xstates */
		__builtin_ia32_xsavec64(xsave_buf, active_xstates); 	/* save state */
#endif

#ifdef CONFIG_LAME_TSC
		/* increment total xsave LAMEs counter */
		k->lame_bundle.total_xsave_cycles += __rdtsc() - tsc_start;
		k->lame_bundle.total_xsave_lames++; 
#endif

		/* Call __lame_jmp_thread_direct to perform context switch */
		__lame_jmp_thread_direct(&cur_th->tf, &next_th->tf,
					 next_th->fsbase);

#ifdef CONFIG_LAME_TSC
		tsc_start = __rdtsc();
#endif

		/* This point is reached when switching back to this thread */
		/* restore xsave state */
		__builtin_ia32_xrstor64(xsave_buf, active_xstates); 
	
#ifdef CONFIG_LAME_TSC
		k->lame_bundle.total_xsave_cycles += __rdtsc() - tsc_start;
#endif
	}	
	else {
		/* LAME re-triggered too early? */
		// uint64_t tsc_now = __rdtsc();
		// if (unlikely(tsc_now - next_th->lame_last_tsc < cfg_emulation_lame_stall_cycles)) {
		// 	// _tpause(0, tsc_now + cfg_emulation_lame_stall_cycles - 400); // tpause here causes hanging
		// 	k->lame_bundle.total_early_lames++; 
		// }
		// next_th->lame_last_tsc = tsc_now - 200;

#ifdef CONFIG_LAME_TSC
		/* increment total LAMEs counter */
		k->lame_bundle.total_cycles += __rdtsc() - tsc_reg_start;
		k->lame_bundle.total_lames++; 
#endif

		/* Call __lame_jmp_thread_direct to perform context switch */
		__lame_jmp_thread_direct(&cur_th->tf, &next_th->tf,
					 next_th->fsbase);
	}
}

__always_inline __nofp void lame_handle_bret(uint64_t *ret) {

	log_warn("[LAME][func:lame_handle_bret] ret=0x%lx", *(ret+8));

}

__always_inline __nofp void lame_stall(void) {
#ifdef CONFIG_LAME_TSC
	struct kthread *k = perthread_read(mykthread);
	k->lame_bundle.total_stall_lames++;
	uint64_t tsc_start = __rdtsc();
#endif
	if (cfg_emulation_lame_stall_cycles > 0) {
    	_tpause(0, __rdtsc() + cfg_emulation_lame_stall_cycles);
	}
#ifdef CONFIG_LAME_TSC
	k->lame_bundle.total_stall_cycles += __rdtsc() - tsc_start;
#endif
}

#ifdef CONFIG_LAME_TSC
__always_inline __nofp void lame_skip(void) {
	struct kthread *k = perthread_read(mykthread);
	k->lame_bundle.total_skip_lames++;
}
#endif

__always_inline __nofp void lame_handle_bret_slowpath(void) {
	struct kthread *k;
	unsigned char *xsave_buf;
	unsigned long active_xstates;

	STAT(PREEMPTIONS)++;

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		return;
	}

	k = getk();

	/* allocate buffer for xsave area on stack */
	xsave_buf = alloca(xsave_max_size + 64);
	xsave_buf = (unsigned char *)align_up((uintptr_t)xsave_buf, 64);

	/* zero xsave header */
	__builtin_memset(xsave_buf + 512, 0, 64);

	active_xstates = __builtin_ia32_xgetbv(0);

	/* save state */
	__builtin_ia32_xsavec64(xsave_buf, active_xstates);

	/* increment total LAMEs counter */
	k->lame_bundle.total_lames++; 

	bool do_cede = preempt_cede_needed(k);
	if (do_cede) {
		thread_cede();
	} else {
		putk();
		thread_yield();
	}

	/* restore state */
	__builtin_ia32_xrstor64(xsave_buf, active_xstates);
}

void lame_print_tsc_counters(void)
{
	uint64_t total[5] = {0}; /* lame; xsave; early; skip; stall */
	unsigned int i;
	for (i = 0; i < maxks; i++) {
		struct kthread *k = ks[i];
		if (!k)
			continue;
		total[0] += k->lame_bundle.total_lames;
		total[1] += k->lame_bundle.total_xsave_lames;
		total[2] += k->lame_bundle.total_early_lames;
		total[3] += k->lame_bundle.total_skip_lames;
		total[4] += k->lame_bundle.total_stall_lames;
		log_warn("[LAME][TSC][kthread:%u] lames=[%lu,%lu,%lu]; [xsave=%lu,%lu,%lu]; early=%lu; skip=%lu; [stall=%lu,%lu,%lu]", i,
				 k->lame_bundle.total_lames,
				 k->lame_bundle.total_lames? k->lame_bundle.total_cycles / k->lame_bundle.total_lames : 0, 
				 k->lame_bundle.total_cycles, 
				 /* xsave stats start */
				 k->lame_bundle.total_xsave_lames,
				 k->lame_bundle.total_xsave_lames? k->lame_bundle.total_xsave_cycles / k->lame_bundle.total_xsave_lames : 0, 
				 k->lame_bundle.total_xsave_cycles, 
				 /* xsave stats end */
				 k->lame_bundle.total_early_lames, 
				 k->lame_bundle.total_skip_lames, 
				 /* stall stats start */
				 k->lame_bundle.total_stall_lames, 
				 k->lame_bundle.total_stall_lames? k->lame_bundle.total_stall_cycles / k->lame_bundle.total_stall_lames : 0, 
				 k->lame_bundle.total_stall_cycles);
#ifdef CONFIG_DEBUG
		for (int j = 0; j < 10; j++) {
			log_warn("[LAME][kthread:%u] rip=%lx; decision=%d", i, 
				perthread_read(rips[j]), perthread_read(decisions[j]));
		}
#endif
	}
	log_warn("[LAME][TSC] total=[%lu,%lu,%lu,%lu,%lu]", total[0], total[1], total[2], total[3], total[4]);
}
/* end */