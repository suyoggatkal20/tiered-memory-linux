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
};

SEC("socket")
int custom_policy(struct tiered_mem_ebpf_ctx *ctx)
{
	if (ctx->nid == 1) {
		if (ctx->access_count >= 3) {
			return 1; /* Promote */
		}
	} else if (ctx->nid == 0) {
		if (ctx->access_count == 0) {
			return 2; /* Demote */
		}
	}
	return 0; /* Keep */
}

char _license[] SEC("license") = "GPL";
