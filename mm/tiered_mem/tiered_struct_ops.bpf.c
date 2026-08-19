#include <linux/bpf.h>

#ifndef SEC
#define SEC(NAME) __attribute__((section(NAME), used))
#endif

struct tiered_mem_ebpf_ctx {
	unsigned long long pfn;
	unsigned int nid;
	unsigned int access_count;
	unsigned int is_lru;
	unsigned int is_active;
	unsigned int page_order;
	unsigned int is_referenced;
	unsigned int is_dirty;
	unsigned int is_writeback;
	unsigned long long zone_free_pages;
	unsigned long long node_total_pages;
};

struct tiered_mem_ops {
	int (*init)(struct tiered_mem_ops *ops);
	void (*track_access)(struct tiered_mem_ebpf_ctx *ctx);
	void (*age_page)(unsigned long pfn);
	int (*classify_page)(struct tiered_mem_ebpf_ctx *ctx);
	int (*get_hot_pages)(int page_count, void *list);
	int (*get_cold_pages)(int page_count, void *list);
	char name[16];
	void *owner;
};

static long (*bpf_trace_printk)(const char *fmt, unsigned int fmt_size, ...) = (void *) BPF_FUNC_trace_printk;
static void *(*bpf_tiered_mem_create_page_counters)(void) = (void *) 212;
static int (*bpf_tiered_mem_inc_page_counter)(void *array_ptr, unsigned long pfn) = (void *) 213;
static int (*bpf_tiered_mem_get_page_counter)(void *array_ptr, unsigned long pfn) = (void *) 214;
static int (*bpf_tiered_mem_decay_page_counter)(void *array_ptr, unsigned long pfn, unsigned int factor) = (void *) 215;

#define bpf_printk(fmt, ...) \
({ \
	char ____fmt[] = fmt; \
	bpf_trace_printk(____fmt, sizeof(____fmt), ##__VA_ARGS__); \
})

void *my_page_counters = 0;


SEC("struct_ops/init")
int tiered_init(struct tiered_mem_ops *ops)
{
	my_page_counters = bpf_tiered_mem_create_page_counters();
	bpf_printk("tiered_mem: struct_ops initialized, custom page_counters array created at %p\n", my_page_counters);
	return 0;
}


SEC("struct_ops/track_access")
void tiered_track_access(struct tiered_mem_ebpf_ctx *ctx)
{
	int new_count;

	if (!ctx || !my_page_counters)
		return;

	new_count = bpf_tiered_mem_inc_page_counter(my_page_counters, ctx->pfn);
	// bpf_printk("tiered_mem: page %llu (nid %u) updated to count %d\n",
	// 	   ctx->pfn, ctx->nid, new_count);
}

SEC("struct_ops/age_page")
void tiered_age_page(unsigned long pfn)
{
	unsigned int ebpf_ageing_factor = 50; 

	if (!my_page_counters)
		return;

	bpf_tiered_mem_decay_page_counter(my_page_counters, pfn, ebpf_ageing_factor);
}


SEC("struct_ops/classify_page")
int tiered_classify_page(struct tiered_mem_ebpf_ctx *ctx)
{
	int count;

	if (!ctx || !my_page_counters)
		return 0;

	count = bpf_tiered_mem_get_page_counter(my_page_counters, ctx->pfn);
	if (count >= 10)
		{bpf_printk("tiered_mem: page %llu (nid %u) is hot\n",
			   ctx->pfn, ctx->nid);
		return 1; /* Promote */}
	else if (count >= 3 ){
		bpf_printk("tiered_mem: page %llu (nid %u) is cold\n",
			   ctx->pfn, ctx->nid);
		return 2; /* Demote */}

	return 0;
}

SEC(".struct_ops")
struct tiered_mem_ops custom_pebs_ops = {
	.init = (void *)tiered_init,
	.track_access = (void *)tiered_track_access,
	.age_page = (void *)tiered_age_page,
	.classify_page = (void *)tiered_classify_page,
	.name = "custom_pebs",
};

char _license[] SEC("license") = "GPL";
