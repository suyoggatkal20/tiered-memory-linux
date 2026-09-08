#include <linux/bpf.h>

#ifndef SEC
#define SEC(NAME) __attribute__((section(NAME), used))
#endif

static long (*bpf_trace_printk)(const char *fmt, unsigned int fmt_size, ...) = (void *) 6;

#ifndef bpf_printk
#define bpf_printk(fmt, ...) \
({ \
    char ____fmt[] = fmt; \
    bpf_trace_printk(____fmt, sizeof(____fmt), ##__VA_ARGS__); \
})
#endif

struct tiered_mem_ebpf_ctx {
	unsigned long long pfn;
	unsigned int nid;
	unsigned int access_count;
	unsigned int is_lru;
	unsigned int is_active;
};

SEC("socket")
int custom_policy(struct tiered_mem_ebpf_ctx *ctx)
{
	bpf_printk("BPF: PFN %llu, Node %u, Access %u\n", ctx->pfn, ctx->nid, ctx->access_count);
	if (ctx->nid == 1) {
		if (ctx->access_count >= 3) {
			bpf_printk("BPF: Promote PFN %llu\n", ctx->pfn);
			return 1; /* Promote */
		}
	} else if (ctx->nid == 0) {
		if (ctx->access_count == 0) {
			bpf_printk("BPF: Demote PFN %llu\n", ctx->pfn);
			return 2; /* Demote */
		}
	}
	return 0; /* Keep */
}

char _license[] SEC("license") = "GPL";
