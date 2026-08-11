/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tiered Memory Custom struct_ops eBPF Program
 *
 * Custom page access tracking via struct_ops hooks (init, track_access).
 * During init, creates a new page counters array via helper 1001.
 * During track_access, updates the element for the given PFN in the custom array via helper 1002.
 *
 * Compile:
 *   clang -g -O2 -target bpf -I/usr/include/x86_64-linux-gnu -c tiered_struct_ops.bpf.c -o tiered_struct_ops.bpf.o
 */

#include <linux/bpf.h>

#ifndef SEC
#define SEC(NAME) __attribute__((section(NAME), used))
#endif

struct tiered_mem_ops {
	int (*init)(struct tiered_mem_ops *ops);
	void (*track_access)(unsigned long pfn);
	int (*get_hot_pages)(int page_count, void *list);
	int (*get_cold_pages)(int page_count, void *list);
	char name[16];
	void *owner;
};

/* BPF Helper function prototypes */
static long (*bpf_trace_printk)(const char *fmt, unsigned int fmt_size, ...) = (void *) BPF_FUNC_trace_printk;
static void *(*bpf_tiered_mem_create_page_counters)(void) = (void *) 212;
static int (*bpf_tiered_mem_inc_page_counter)(void *array_ptr, unsigned long pfn) = (void *) 213;
static int (*bpf_tiered_mem_get_page_counter)(void *array_ptr, unsigned long pfn) = (void *) 214;

#define bpf_printk(fmt, ...) \
({ \
	char ____fmt[] = fmt; \
	bpf_trace_printk(____fmt, sizeof(____fmt), ##__VA_ARGS__); \
})

/* Global variable to hold the allocated custom page counters array pointer */
void *my_page_counters = 0;

/*
 * Hook 1: init
 * Runs when struct_ops is registered. Creates a new custom array for page tracking.
 */
SEC("struct_ops/init")
int tiered_init(struct tiered_mem_ops *ops)
{
	my_page_counters = bpf_tiered_mem_create_page_counters();
	bpf_printk("tiered_mem: struct_ops initialized, custom page_counters array created at %p\n", my_page_counters);
	return 0;
}

/*
 * Hook 2: track_access
 * Triggered by pebs.c on page access.
 * Takes the user's custom array pointer and pfn to update access count.
 */
SEC("struct_ops/track_access")
void tiered_track_access(unsigned long pfn)
{
	int new_count;

	if (!my_page_counters)
		return;

	new_count = bpf_tiered_mem_inc_page_counter(my_page_counters, pfn);
	bpf_printk("tiered_mem: page access updated in custom array to count %d\n", new_count);
}

SEC(".struct_ops")
struct tiered_mem_ops custom_pebs_ops = {
	.init = (void *)tiered_init,
	.track_access = (void *)tiered_track_access,
	.name = "custom_pebs",
};

char _license[] SEC("license") = "GPL";
