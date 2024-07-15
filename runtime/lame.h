/*
 * lame.h - LAME (Latency-Aware Memory Exception) offset definitions
 * 
 * This header defines the absolute byte offsets for LAME bundle structures.
 * These values are used by assembly code in lame.S and must match the
 * actual structure layouts defined in defs.h.
 * 
 * WARNING: These values reflect the layout of struct lame_bundle and
 * struct lame_uthread_wrapper. Don't change these values without also
 * updating the corresponding structures in defs.h otherwise build will fail.
 */

#pragma once

/*
 * LAME Bundle Structure Offsets
 * 
 * struct lame_bundle {
 *     struct lame_uthread_wrapper uthreads[8];  // 8 * 25 = 200 bytes
 *     unsigned int size;                         // 4 bytes
 *     unsigned int used;                         // 4 bytes  
 *     unsigned int active;                       // 4 bytes
 *     uint64_t total_cycles;                     // 8 bytes
 *     uint64_t total_lames;                      // 8 bytes
 *     uint64_t total_xsave_lames;                // 8 bytes
 *     bool enabled;                              // 1 byte + 7 padding
 * };
 */

/* Bundle field offsets */
#define LAME_BUNDLE_UTHREADS        (0)     /* array of uthread wrappers */
#define LAME_BUNDLE_SIZE            (256)   /* configured bundle size */
#define LAME_BUNDLE_USED            (260)   /* number of occupied slots */
#define LAME_BUNDLE_ACTIVE          (264)   /* current running uthread index */
#define LAME_BUNDLE_TOTAL_CYCLES    (272)   /* total cycles across all uthreads */
#define LAME_BUNDLE_TOTAL_LAMES     (280)   /* total LAMEs handled */
#define LAME_BUNDLE_ENABLED         (296)   /* dynamic runtime enable/disable flag */

/*
 * LAME Uthread Wrapper Structure Offsets
 * 
 * struct lame_uthread_wrapper {
 *     thread_t *uthread;    // 8 bytes
 *     bool present;         // 1 byte
 *     uint64_t cycles;      // 8 bytes  
 *     uint64_t lame_count;  // 8 bytes
 * };
 * Total size: 32 bytes (with 7 bytes padding to align to 8-byte boundary)
 */

/* Uthread wrapper field offsets */
#define LAME_UTHREAD_WRAPPER_UTHREAD    (0)     /* pointer to the actual uthread */
#define LAME_UTHREAD_WRAPPER_PRESENT    (8)     /* whether this slot is occupied */
#define LAME_UTHREAD_WRAPPER_CYCLES     (16)    /* accounting: cycles executed */
#define LAME_UTHREAD_WRAPPER_LAME_COUNT (24)    /* accounting: number of LAMEs handled */
#define LAME_UTHREAD_WRAPPER_SIZE       (32)    /* total size with padding */

/*
 * Kthread Structure - LAME Bundle Offset
 * 
 * The lame_bundle is located at the 11th cache line (offset 0x300 = 768 bytes)
 * from the start of the kthread structure.
 */

#define LAME_BUNDLE_OFFSET          (0x300)  /* offset of lame_bundle in kthread */

/*
 * Thread Structure - Trapframe Offset
 * 
 * The thread_tf is located at offset 112 from the start of the thread structure.
 */

#define THREAD_TF_OFFSET            (112)    /* offset of tf in thread_t */


/*
 * Red zone
 */
#define LAME_RZ_BYTES               128   /* x86-64 SysV ABI */
