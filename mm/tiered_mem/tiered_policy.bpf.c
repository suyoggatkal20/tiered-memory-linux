/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tiered Memory eBPF Policy Program
 *
 * Compiled with: clang -O2 -target bpf -c tiered_policy.bpf.c -o tiered_policy.bpf.o
 *
 * Returns:
 *   0 = KEEP     — no action
 *   1 = PROMOTE  — CXL → DRAM
 *   2 = DEMOTE   — DRAM → CXL
 */

/* Do NOT use vmlinux.h — define the struct manually to avoid
 * BTF version mismatches with older libbpf */

#ifndef SEC
#define SEC(NAME) __attribute__((section(NAME), used))
#endif

/* Must match the kernel's struct tiered_mem_ebpf_ctx exactly */
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

#define TIERED_MEM_KEEP    0
#define TIERED_MEM_PROMOTE 1
#define TIERED_MEM_DEMOTE  2

#define HOT_THRESHOLD       3
#define COLD_THRESHOLD      1
#define EMERGENCY_HOT_MULT  4
#define PRESSURE_PCT        5

SEC("tiered_mem")
int tiered_mem_policy(struct tiered_mem_ebpf_ctx *ctx)
{
	unsigned int access  = ctx->access_count;
	unsigned int active  = ctx->is_active;
	unsigned int lru     = ctx->is_lru;
	unsigned int ref     = ctx->is_referenced;
	unsigned int dirty   = ctx->is_dirty;
	unsigned int wb      = ctx->is_writeback;
	unsigned long long free_pgs  = ctx->zone_free_pages;
	unsigned long long total_pgs = ctx->node_total_pages;

	/*
	 * ── Promotion path (CXL → DRAM) ──
	 * Called from get_hot_pages() which scans CXL nodes.
	 */

	/* Emergency fast-path: extremely hot page, always promote */
	if (access >= HOT_THRESHOLD * EMERGENCY_HOT_MULT)
		return TIERED_MEM_PROMOTE;

	/* Normal promotion: hot + on LRU + (active or referenced) + not writeback */
	if (access >= HOT_THRESHOLD) {
		if (!lru)
			return TIERED_MEM_KEEP;
		if (!active && !ref)
			return TIERED_MEM_KEEP;
		if (wb)
			return TIERED_MEM_KEEP;

		/* Check DRAM has enough free memory (> 5% of total) */
		if (total_pgs > 0) {
			unsigned long long threshold = total_pgs * PRESSURE_PCT / 100;
			if (free_pgs < threshold)
				return TIERED_MEM_KEEP;
		}

		return TIERED_MEM_PROMOTE;
	}

	/*
	 * ── Demotion path (DRAM → CXL) ──
	 * Called from get_cold_pages() which scans DRAM nodes.
	 */

	/* Under DRAM pressure, be more aggressive */
	unsigned int effective_cold = COLD_THRESHOLD;
	if (total_pgs > 0) {
		unsigned long long pressure_line = total_pgs * PRESSURE_PCT / 100;
		if (free_pgs < pressure_line)
			effective_cold = COLD_THRESHOLD * 2;
	}

	if (access <= effective_cold) {
		if (active)
			return TIERED_MEM_KEEP;
		if (ref)
			return TIERED_MEM_KEEP;
		if (dirty)
			return TIERED_MEM_KEEP;

		return TIERED_MEM_DEMOTE;
	}

	return TIERED_MEM_KEEP;
}

char _license[] SEC("license") = "GPL";
