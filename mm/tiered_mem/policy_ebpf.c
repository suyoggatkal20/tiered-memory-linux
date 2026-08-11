/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tiered Memory eBPF Policy Program
 *
 * This BPF program runs inside the kernel as part of the tiered memory
 * framework.  For every candidate page the ktierd scanner evaluates,
 * this program is called with a tiered_mem_ebpf_ctx and must return:
 *
 *   0 (KEEP)    – leave the page where it is
 *   1 (PROMOTE) – migrate the page from CXL → DRAM
 *   2 (DEMOTE)  – migrate the page from DRAM → CXL
 *
 * ── Decision Algorithm ──────────────────────────────────────────────
 *
 * Promotion (page is currently on CXL):
 *   A page is promoted to DRAM when ALL of the following are true:
 *     • access_count >= HOT_THRESHOLD  (page is "hot")
 *     • The page is on the LRU and is either active or referenced
 *     • The page is NOT currently under writeback
 *     • The DRAM zone has enough free pages (pressure check)
 *   A page that is extremely hot (access_count >= 4 × HOT_THRESHOLD)
 *   is promoted unconditionally (emergency fast-path).
 *
 * Demotion (page is currently on DRAM):
 *   A page is demoted to CXL when ALL of the following are true:
 *     • access_count <= COLD_THRESHOLD  (page is "cold")
 *     • The page is NOT active on the LRU
 *     • The page is NOT referenced (no recent second-chance)
 *     • The page is NOT dirty (avoid write-back stalls)
 *   Additionally, if DRAM memory pressure is high (free pages < 5%
 *   of total), the cold threshold is relaxed by 2× to be more
 *   aggressive about reclaiming DRAM space.
 *
 * ── Compile ─────────────────────────────────────────────────────────
 *   clang -O2 -target bpf -c policy_ebpf.c -o policy_ebpf.o
 *
 * ── Load ────────────────────────────────────────────────────────────
 *   ./loader policy_ebpf.o
 */
#include <linux/bpf.h>

#ifndef SEC
#define SEC(NAME) __attribute__((section(NAME), used))
#endif

/* ── Return codes ──────────────────────────────────────────────────── */
#define TIERED_MEM_KEEP    0
#define TIERED_MEM_PROMOTE 1
#define TIERED_MEM_DEMOTE  2

/* ── Tunables ──────────────────────────────────────────────────────── */
/*
 * These are compiled-in defaults.  The kernel's sysfs hot_threshold /
 * cold_threshold can be queried at runtime via BPF helpers if the
 * program needs to track live changes.
 */
#define HOT_THRESHOLD       3    /* min access_count to consider hot    */
#define COLD_THRESHOLD      1    /* max access_count to consider cold   */
#define EMERGENCY_HOT_MULT  4    /* multiplier for unconditional promote*/
#define PRESSURE_PCT        5    /* free-page % below which is "pressure" */

/* ── Context structure (must match the UAPI definition) ──────────── */
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

SEC("tiered_mem")
int tiered_mem_policy(struct tiered_mem_ebpf_ctx *ctx)
{
	unsigned int access  = ctx->access_count;
	unsigned int nid     = ctx->nid;
	unsigned int active  = ctx->is_active;
	unsigned int lru     = ctx->is_lru;
	unsigned int ref     = ctx->is_referenced;
	unsigned int dirty   = ctx->is_dirty;
	unsigned int wb      = ctx->is_writeback;
	unsigned long long free_pgs  = ctx->zone_free_pages;
	unsigned long long total_pgs = ctx->node_total_pages;

	/*
	 * ──────────────────────────────────────────────────────────
	 *  Promotion path  (CXL → DRAM)
	 *
	 *  This function is called from get_hot_pages() which only
	 *  scans CXL nodes, so nid is always a CXL node here.
	 *  We check access_count and folio state to decide.
	 * ──────────────────────────────────────────────────────────
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

		/*
		 * Memory pressure check on the *destination* DRAM tier.
		 * We use zone_free_pages from the page's current zone to
		 * estimate general system pressure.  If free memory is
		 * less than PRESSURE_PCT of total, skip promotion to
		 * avoid pushing DRAM into reclaim.
		 *
		 * Note: total_pgs == 0 is a guard against division-by-zero
		 * in degenerate configurations.
		 */
		if (total_pgs > 0) {
			unsigned long long threshold = total_pgs * PRESSURE_PCT / 100;
			if (free_pgs < threshold)
				return TIERED_MEM_KEEP;
		}

		return TIERED_MEM_PROMOTE;
	}

	/*
	 * ──────────────────────────────────────────────────────────
	 *  Demotion path  (DRAM → CXL)
	 *
	 *  This function is called from get_cold_pages() which only
	 *  scans DRAM nodes.  We look for cold, inactive, clean
	 *  pages that are safe to push down to CXL.
	 * ──────────────────────────────────────────────────────────
	 */

	/* Determine effective cold threshold */
	unsigned int effective_cold = COLD_THRESHOLD;

	/*
	 * Under DRAM pressure (free < 5% of total), relax the cold
	 * threshold to be more aggressive about demotion.
	 */
	if (total_pgs > 0) {
		unsigned long long pressure_line = total_pgs * PRESSURE_PCT / 100;
		if (free_pgs < pressure_line)
			effective_cold = COLD_THRESHOLD * 2;
	}

	if (access <= effective_cold) {
		/* Active pages still have value; keep them in DRAM */
		if (active)
			return TIERED_MEM_KEEP;

		/* Recently referenced pages get a second chance */
		if (ref)
			return TIERED_MEM_KEEP;

		/*
		 * Dirty pages would require a writeback before migration
		 * could complete — avoid stalling the ktierd scanner.
		 */
		if (dirty)
			return TIERED_MEM_KEEP;

		return TIERED_MEM_DEMOTE;
	}

	/* Default: page is neither hot enough nor cold enough — keep it */
	return TIERED_MEM_KEEP;
}

char _license[] SEC("license") = "GPL";
