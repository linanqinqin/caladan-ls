/*
 * init.c - initializes the runtime
 */

#include <pthread.h>

#include <base/cpu.h>
#include <base/init.h>
#include <base/log.h>
#include <base/limits.h>
#include <runtime/thread.h>

/* linanqinqin */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/lame.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <unistd.h>
/* External configuration variable */
extern unsigned int cfg_lame_bundle_size;
extern unsigned int cfg_lame_tsc;
extern unsigned int cfg_lame_register;
extern unsigned int cfg_emulation_lame_stall_cycles;
/* end */

#include "defs.h"

static pthread_barrier_t init_barrier;

struct init_entry {
	const char *name;
	int (*init)(void);
};

static initializer_fn_t global_init_hook = NULL;
static initializer_fn_t perthread_init_hook = NULL;
static initializer_fn_t late_init_hook = NULL;


#define GLOBAL_INITIALIZER(name) \
	{__cstr(name), &name ## _init}

/* global subsystem initialization */
static const struct init_entry global_init_handlers[] = {
	/* runtime core */
	GLOBAL_INITIALIZER(kthread),
	GLOBAL_INITIALIZER(ioqueues),
	GLOBAL_INITIALIZER(runtime_stack),
	GLOBAL_INITIALIZER(sched),
	GLOBAL_INITIALIZER(preempt),
	GLOBAL_INITIALIZER(smalloc),

	/* network stack */
	GLOBAL_INITIALIZER(net),
	GLOBAL_INITIALIZER(udp),
	GLOBAL_INITIALIZER(directpath),
	GLOBAL_INITIALIZER(arp),
	GLOBAL_INITIALIZER(trans),

	/* storage */
	GLOBAL_INITIALIZER(storage),

#ifdef GC
	GLOBAL_INITIALIZER(gc),
#endif
};

#define THREAD_INITIALIZER(name) \
	{__cstr(name), &name ## _init_thread}

/* per-kthread subsystem initialization */
static const struct init_entry thread_init_handlers[] = {
	/* runtime core */
	THREAD_INITIALIZER(stack),
	THREAD_INITIALIZER(preempt),
	THREAD_INITIALIZER(kthread),
	THREAD_INITIALIZER(ioqueues),
	THREAD_INITIALIZER(sched),
	/* linanqinqin original code */
	// THREAD_INITIALIZER(timer),
	/* end */
	THREAD_INITIALIZER(smalloc),
	/* linanqinqin */
	THREAD_INITIALIZER(timer),
	/* end */

	/* network stack */
	THREAD_INITIALIZER(net),
	THREAD_INITIALIZER(directpath),

	/* storage */
	THREAD_INITIALIZER(storage),
};

#define LATE_INITIALIZER(name) \
	{__cstr(name), &name ## _init_late}

static const struct init_entry late_init_handlers[] = {
	/* network stack */
	LATE_INITIALIZER(arp),
	LATE_INITIALIZER(stat),
	LATE_INITIALIZER(tcp),
	LATE_INITIALIZER(rcu),
	LATE_INITIALIZER(directpath),
};

static int run_init_handlers(const char *phase,
			     const struct init_entry *h, int nr)
{
	int i, ret;

	log_debug("entering '%s' init phase", phase);
	for (i = 0; i < nr; i++) {
		log_debug("init -> %s", h[i].name);
		ret = h[i].init();
		if (ret) {
			log_debug("failed, ret = %d", ret);
			return ret;
		}
	}

	return 0;
}

static int runtime_init_thread(void)
{
	int ret;

	ret = base_init_thread();
	if (ret) {
		log_err("base library per-thread init failed, ret = %d", ret);
		return ret;
	}

	ret = run_init_handlers("per-thread", thread_init_handlers,
				 ARRAY_SIZE(thread_init_handlers));
	if (ret || perthread_init_hook == NULL)
		return ret;

	return perthread_init_hook();

}

static void *pthread_entry(void *data)
{
	int ret;

	ret = runtime_init_thread();
	BUG_ON(ret);

	pthread_barrier_wait(&init_barrier);
	pthread_barrier_wait(&init_barrier);
	sched_start();

	/* never reached unless things are broken */
	BUG();
	return NULL;
}

/* linanqinqin */
uint64_t text_section_start = 0;
uint64_t text_section_end = 0;

unsigned char *avx_bitmap = NULL;
uint64_t avx_bitmap_size = 0;

unsigned char *gpr_bitmap = NULL;
uint64_t gpr_bitmap_size = 0;

static int readlink_exe(char *buf, size_t sz) {
	ssize_t n = readlink("/proc/self/exe", buf, sz - 1);
	if (n < 0) return -1;
	buf[n] = '\0';
	return 0;
}

// Return text mapping [start,end) of the main executable from /proc/self/maps
static int get_main_exec_text_range(uint64_t *start_out, uint64_t *end_out) {

    char exe_path[PATH_MAX];
    if (readlink_exe(exe_path, sizeof(exe_path)) != 0) {
		return -1;
	}
	
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
		return -1;
	}

    char line[4096];
    while (fgets(line, sizeof(line), maps)) {
        uint64_t start = 0, end = 0;
        char perms[8] = {0};
        unsigned long offset = 0;

        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %lx", &start, &end, perms, &offset) != 4) continue;
        if (perms[0] != 'r' || perms[2] != 'x') continue;
        
		char *path = strchr(line, '/');
        if (!path) continue;
        
		size_t l = strlen(path);
        if (l && path[l - 1] == '\n') { 
			path[l - 1] = '\0'; 
		}

        char exe_buf[PATH_MAX];
        if (readlink_exe(exe_buf, sizeof(exe_buf)) != 0) continue;
        if (strcmp(path, exe_buf) != 0) continue;

        fclose(maps);

        *start_out = start;
        *end_out = end; // end is exclusive as per /proc/self/maps
		
        return 0;
    }
    fclose(maps);
    return -1;
}

static int load_sessions(const char *file, uint64_t **starts, uint64_t **ends, size_t *count) {

	FILE *f = fopen(file, "rb");
	if (!f) return -1;

	if (fseek(f, 0, SEEK_END) != 0) { 
		fclose(f); 
		return -1; 
	}

	long sz = ftell(f);
	if (sz < 0) { 
		fclose(f); 
		return -1; 
	}
	rewind(f);
	if (sz % 16 != 0) { 
		fclose(f); 
		return -1; 
	}

	size_t cnt = (size_t)(sz / 16);
	uint64_t *s = (uint64_t*)calloc(cnt, sizeof(uint64_t));
	uint64_t *e = (uint64_t*)calloc(cnt, sizeof(uint64_t));
	if (!s || !e) { 
		fclose(f); 
		free(s); 
		free(e); 
		return -1; 
	}

	for (size_t i = 0; i < cnt; i++) {
		uint64_t rs = 0, re = 0;
		if (fread(&rs, sizeof(uint64_t), 1, f) != 1 || fread(&re, sizeof(uint64_t), 1, f) != 1) {
			fclose(f); 
			free(s); 
			free(e); 
			return -1;
		}
		s[i] = rs;
		e[i] = re;
#ifdef CONFIG_DEBUG
		if (i < 10) {
			log_info("[LAME][load_sessions] session %lu: start = 0x%lx, end = 0x%lx", i, rs, re);
		}
#endif
	}
	fclose(f);

	*starts = s; 
	*ends = e; 
	*count = cnt;

	return 0;
}

static int gpr_bitmap_init()
{
	// 1) Determine full path to executable
    char exe_path[PATH_MAX];
    if (readlink_exe(exe_path, sizeof(exe_path)) != 0) {
		log_err("[LAME][gpr_bitmap_init] readlink_exe failed: %d", errno);
        return -errno;
    }

    // 2) Build avxdump path: <exe_path>.avxdump
    char gpr_path[PATH_MAX];
    size_t elen = strlen(exe_path);
    if (elen + strlen(".gprdump") + 1 >= sizeof(gpr_path)) {
        log_err("[LAME][gpr_bitmap_init] gprdump path too long");
        return -ENAMETOOLONG;
    }
    snprintf(gpr_path, sizeof(gpr_path), "%s.gprdump", exe_path);

    // 3) Read sessions (RVAs) from avxdump file (headerless pairs of uint64)
    uint64_t *rel_starts = NULL, *rel_ends = NULL; 
	size_t count = 0;
    if (load_sessions(gpr_path, &rel_starts, &rel_ends, &count) != 0) {
        log_err("[LAME][gpr_bitmap_init] failed to read gpr sessions from %s", gpr_path);
        return -errno;
    }

    // 4) Get runtime text mapping range for the main executable from /proc/self/maps
    uint64_t text_start = 0, text_end = 0;
    if (get_main_exec_text_range(&text_start, &text_end) != 0) {
        log_err("[LAME][gpr_bitmap_init] failed to get runtime text range: %d", errno);
        free(rel_starts);
        free(rel_ends);
        return -errno;
    }

	// 5) Build page bitmap (1 byte per page, default page_size=64 configurable via AVX_PAGE_SIZE)
    uint64_t pgsz_factor = LAME_BITMAP_PGSZ_FACTOR;
    uint64_t text_len = (text_end > text_start) ? (text_end - text_start) : 0;
    uint64_t num_pages = (text_len >> pgsz_factor) + 1;
    unsigned char *bitmap = NULL;
    if (num_pages > 0) {
		bitmap = (unsigned char*)calloc((num_pages>>LAME_BITMAP_BYTE_SHIFT)+1, 1);
	}
    if (bitmap) {
        // mark pages: sessions are inclusive on both ends
        for (size_t i = 0; i < count; i++) {
			uint64_t s = rel_starts[i];
			uint64_t e = rel_ends[i];

			if (e <= s) continue; // skip invalid sessions
			e = (e+text_start >= text_end) ? text_end : e; // clamp end if out of range (should never happen though)

			uint64_t start_idx = (s & ((1UL<<pgsz_factor)-1)) ? (s>>pgsz_factor)+1 : s>>pgsz_factor;
			uint64_t end_idx = (e>>pgsz_factor)-1; 
			if (end_idx >= num_pages) end_idx = num_pages - 1;

			for (uint64_t p = start_idx; p <= end_idx; p++) {
				bitmap[p >> LAME_BITMAP_BYTE_SHIFT] |= (1UL << (p & LAME_BITMAP_BYTE_MASK));
			}
#ifdef CONFIG_DEBUG
			if (i < 10) {
				log_info("[LAME][gpr_bitmap_init] session %lu: start = 0x%lx, end = 0x%lx, start_idx = %lu, end_idx = %lu", 
					i, s, e, start_idx, end_idx);
			}
#endif
        }

		gpr_bitmap = bitmap;
		text_section_start = text_start;
		text_section_end = text_end;
		gpr_bitmap_size = num_pages;
		free(rel_starts); 
		free(rel_ends);

        log_info("[LAME] gpr bitmap has %lu pages, page size = %lu bytes, start = 0x%lx, end = 0x%lx", 
				num_pages, 1UL << pgsz_factor, text_start, text_end);
		
		return 0;
    } else {
        log_err("[LAME] gpr bitmap not allocated (num_pages=%lu)", num_pages);
        free(bitmap); 
		free(rel_starts); 
		free(rel_ends);
        return -EINVAL;
    }
}

/* register lame handler via ioctl */
static int lame_init(void)
{
	int lamedev;
	struct lame_arg arg;
	int ret;
	int register_mode;

	if (cfg_lame_register == RT_LAME_REGISTER_NONE) {
		log_warn("WARNING: LAME handler not registered");
		return 0;
	}

	lamedev = open("/dev/lame", O_RDWR);
    if (lamedev < 0) {
        log_err("[errno %d] Failed to open /dev/lame", errno);
		return lamedev;
    }
    else {
		arg.present = 1;

		/* select the register mode */
		if (cfg_lame_register == RT_LAME_REGISTER_INT) {
			/* via INT*/
			register_mode = LAME_REGISTER_INT;
			if (cfg_lame_bundle_size == 2) {
				arg.handler_addr = (__u64)__lame_entry2;
			} else {
				arg.handler_addr = (__u64)__lame_entry_nop;
			}
		} else if (cfg_lame_register == RT_LAME_REGISTER_PMU) {
			/* via PMU, with LAME switching */
			register_mode = LAME_REGISTER_PMU;
			arg.handler_addr = (__u64)__lame_entry_bret;
		} else if (cfg_lame_register == RT_LAME_REGISTER_STALL) {
			/* via PMU, with stall emulation */
			register_mode = LAME_REGISTER_PMU; /* pmu, stall, nop use the same kernel register */
			arg.handler_addr = (__u64)__lame_entry_stall_bret;
		} else if (cfg_lame_register == RT_LAME_REGISTER_NOP) {
			/* via PMU, with nop */
			register_mode = LAME_REGISTER_PMU; /* pmu, stall, nop use the same kernel register */
			arg.handler_addr = (__u64)__lame_entry_nop_bret;
		}
		else {
			log_err("unsupported LAME register type");
			return -1;
		}
		
		ret = ioctl(lamedev, register_mode, &arg);
		if (ret < 0) {
			log_err("[errno %d] ioctl LAME_REGISTER failed", errno);
		} else {
			char *lame_types[5] = {"none", "int", "pmu", "stall", "nop"};
			log_notice("LAME handler registered at %p [bundle size: %u][mode: %s][stall: %u]", 
				(void *)arg.handler_addr, cfg_lame_bundle_size, 
				// cfg_lame_register == RT_LAME_REGISTER_INT ? "int" : (cfg_lame_register == RT_LAME_REGISTER_PMU ? "pmu" : "stall"),
				lame_types[cfg_lame_register], 
				cfg_emulation_lame_stall_cycles);
		}
		close(lamedev);
	}

	return ret;
}

/* end */

/**
 * runtime_set_initializers - allow runtime to specifcy a function to run in
 * each stage of intialization (called before runtime_init).
 */
int runtime_set_initializers(initializer_fn_t global_fn,
			     initializer_fn_t perthread_fn,
			     initializer_fn_t late_fn)
{
	global_init_hook = global_fn;
	perthread_init_hook = perthread_fn;
	late_init_hook = late_fn;
	return 0;
}

/**
 * runtime_init - starts the runtime
 * @cfgpath: the path to the configuration file
 * @main_fn: the first function to run as a thread
 * @arg: an argument to @main_fn
 *
 * Does not return if successful, otherwise return  < 0 if an error.
 */
int runtime_init(const char *cfgpath, thread_fn_t main_fn, void *arg)
{
	pthread_t tid[NCPU];
	int ret, i;

	ret = ioqueues_init_early();
	if (unlikely(ret))
		return ret;

	cycles_per_us = iok.iok_info->cycles_per_us;

	ret = base_init();
	if (ret) {
		log_err("base library global init failed, ret = %d", ret);
		return ret;
	}

	ret = cfg_load(cfgpath);
	if (ret)
		return ret;

	/* linanqinqin */

	/* construct the bitmap for gpr sessions */
	ret = gpr_bitmap_init();
	if (ret) {
		log_err("gpr bitmap init failed, ret = %d", ret);
		return ret;
	}

	/* register lame handler via ioctl */
	ret = lame_init();
	if (ret) {
		log_warn("WARNING: LAME capability not enabled");
	}

#ifdef DEBUG
	log_info("LAME_BUNDLE_OFFSET: %lu", offsetof(struct kthread, lame_bundle));
	log_info("LAME_BUNDLE_UTHREADS: %lu", offsetof(struct lame_bundle, uthreads));
	log_info("LAME_BUNDLE_SIZE: %lu", offsetof(struct lame_bundle, size));
	log_info("LAME_BUNDLE_USED: %lu", offsetof(struct lame_bundle, used));
	log_info("LAME_BUNDLE_ACTIVE: %lu", offsetof(struct lame_bundle, active));
	log_info("LAME_BUNDLE_TOTAL_CYCLES: %lu", offsetof(struct lame_bundle, total_cycles));
	log_info("LAME_BUNDLE_TOTAL_LAMES: %lu", offsetof(struct lame_bundle, total_lames));
	log_info("LAME_BUNDLE_ENABLED: %lu", offsetof(struct lame_bundle, enabled));
	log_info("LAME_UTHREAD_WRAPPER_UTHREAD: %lu", offsetof(struct lame_uthread_wrapper, uthread));
	log_info("LAME_UTHREAD_WRAPPER_PRESENT: %lu", offsetof(struct lame_uthread_wrapper, present));
	log_info("LAME_UTHREAD_WRAPPER_CYCLES: %lu", offsetof(struct lame_uthread_wrapper, cycles));
	log_info("LAME_UTHREAD_WRAPPER_LAME_COUNT: %lu", offsetof(struct lame_uthread_wrapper, lame_count));
	log_info("LAME_UTHREAD_WRAPPER_SIZE: %lu", sizeof(struct lame_uthread_wrapper));
	log_info("THREAD_TF_OFFSET: %lu", offsetof(struct thread, tf));
#endif
	/* end */
	
	log_info("process pid: %u", getpid());

	pthread_barrier_init(&init_barrier, NULL, maxks);

	ret = run_init_handlers("global", global_init_handlers,
				ARRAY_SIZE(global_init_handlers));
	if (ret)
		return ret;

	if (global_init_hook) {
		ret = global_init_hook();
		if (ret) {
			log_err("User-specificed global initializer failed, ret = %d", ret);
			return ret;
		}
	}

	ret = runtime_init_thread();
	BUG_ON(ret);

	log_info("spawning %d kthreads", maxks);
	for (i = 1; i < maxks; i++) {
		ret = pthread_create(&tid[i], NULL, pthread_entry, NULL);
		BUG_ON(ret);
	}

	pthread_barrier_wait(&init_barrier);

	ret = ioqueues_register_iokernel();
	if (ret) {
		log_err("couldn't register with iokernel, ret = %d", ret);
		return ret;
	}

	pthread_barrier_wait(&init_barrier);

	/* point of no return starts here */

	ret = thread_spawn_main(main_fn, arg);
	BUG_ON(ret);

	ret = run_init_handlers("late", late_init_handlers,
				ARRAY_SIZE(late_init_handlers));
	BUG_ON(ret);

	if (late_init_hook) {
		ret = late_init_hook();
		if (ret) {
			log_err("User-specificed late initializer failed, ret = %d", ret);
			return ret;
		}
	}

	sched_start();

	/* never reached unless things are broken */
	BUG();
	return 0;
}
