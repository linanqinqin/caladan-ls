/*
 * lame.h - LAME bundle scheduling public interface
 */

#pragma once

#include <base/thread.h>

/* Forward declarations */
struct kthread;

/* LAME bundle management functions */
extern int lame_bundle_add_uthread(struct kthread *k, thread_t *th, bool set_active);
extern int lame_bundle_remove_uthread(struct kthread *k, thread_t *th);
extern int lame_bundle_remove_uthread_by_index(struct kthread *k, unsigned int index);
extern int lame_bundle_remove_uthread_at_active(struct kthread *k);
extern unsigned int lame_bundle_get_used_count(struct kthread *k);
extern void lame_sched_bundle_dismantle(struct kthread *k);
extern void lame_sched_bundle_dismantle_nolock(struct kthread *k);

/* LAME scheduling functions */
extern void lame_handle(uint64_t rip);
extern void lame_stall(void);
extern void lame_handle_bret_slowpath(void);

/* Dynamic bundle scheduling control functions */
extern void lame_sched_enable(struct kthread *k);
extern void lame_sched_disable(struct kthread *k);
extern bool lame_sched_is_statically_enabled(struct kthread *k);
extern bool lame_sched_is_dynamically_enabled(struct kthread *k);

/* Internal symbols for testing (not part of public API) */
#ifdef LAME_TESTING
extern unsigned int cfg_lame_bundle_size;
#endif
