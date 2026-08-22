/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/perf_event.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/pgtable.h>
#include <linux/rmap.h>
#include "internal.h"

static DEFINE_PER_CPU(struct perf_event *, tiered_pebs_events);
static atomic_t interval_samples_count;

/*
 * Lockless virtual->PFN walk, safe to call from NMI/PMI context.
 *
 * Unlike the original version, this checks for huge leaf entries at the
 * PUD and PMD levels (1GB / 2MB pages). Without those checks, treating a
 * huge pmd/pud's contents as a pointer to the next-level table would
 * misinterpret the entry and either compute a garbage PFN or crash.
 *
 * Note: this still isn't "lockless-safe" in the strongest sense (no
 * mmap_lock, no page table lock) — READ_ONCE() prevents split/torn reads
 * of a single entry, but a concurrent page-table teardown between our
 * reads at different levels is still possible in theory. That's an
 * inherent limitation of walking page tables from NMI context without
 * arch-specific *_offset_lockless() helpers; it's the same trade-off the
 * kernel's own perf_get_page_size() makes.
 */
static unsigned long virt_to_pfn_lockless(struct mm_struct *mm,
					  unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	unsigned long pfn = 0;

	if (!mm)
		return 0;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;

	pudp = pud_offset(p4d, addr);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud))
		return 0;

	if (pud_leaf(pud)) {
		if (!pud_present(pud))
			return 0;
		return pud_pfn(pud) + ((addr & ~PUD_MASK) >> PAGE_SHIFT);
	}

	if (pud_bad(pud))
		return 0;

	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (pmd_none(pmd))
		return 0;

	if (pmd_leaf(pmd)) {
		if (!pmd_present(pmd))
			return 0;
		return pmd_pfn(pmd) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
	}

	if (pmd_bad(pmd))
		return 0;

	ptep = pte_offset_kernel(pmdp, addr);
	if (!ptep)
		return 0;

	pte = READ_ONCE(*ptep);
	if (pte_present(pte))
		pfn = pte_pfn(pte);

	return pfn;
}

#include "../internal.h"

/*
 * NOTE: the rmap_walk()/anon_vma-based fallback that used to live here
 * has been removed. This handler runs in NMI/PMI context (PEBS overflow),
 * and both rmap_walk() and a correct anon_vma interval-tree lookup take
 * sleeping locks (anon_vma_lock_read() / i_mmap_lock_read()) — taking or
 * even contending on those from NMI context risks deadlocking the CPU if
 * the interrupted context already held the same lock. There is no safe
 * way to resolve pid/tgid from the folio alone in this context, so we
 * rely solely on `current` and leave pid/tgid as 0 otherwise.
 */

void populate_ebpf_ctx(struct tiered_mem_ebpf_ctx *ctx, unsigned long pfn)
{
	struct page *page;
	struct folio *folio = NULL;
	int nid = 0;
	unsigned long long zone_free_pages = 0;
	unsigned long long node_total_pages = 0;
	unsigned int pid = 0, tgid = 0;

	if (pfn >= max_pfn)
		return;

	page = pfn_to_online_page(pfn);
	if (page) {
		/*
		 * page_folio() follows compound_head() internally, so it
		 * already handles tail pages correctly — no need to special
		 * case PageTail() here. The old check skipped folio
		 * resolution (and therefore is_lru/is_active/page_order/
		 * pid fallback) for almost every PFN backed by a THP.
		 */
		folio = page_folio(page);
		nid = page_to_nid(page);
	} else if (pfn_valid(pfn)) {
		nid = pfn_to_nid(pfn);
	}

	if (nid >= 0 && nid < MAX_NUMNODES && node_online(nid)) {
		pg_data_t *pgdat = NODE_DATA(nid);
		if (pgdat) {
			int i;
			node_total_pages = pgdat->node_present_pages;
			for (i = 0; i < MAX_NR_ZONES; i++) {
				struct zone *zone = &pgdat->node_zones[i];
				if (populated_zone(zone))
					zone_free_pages += zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}

	if (current && current->mm && current->pid > 0) {
		pid = current->pid;
		tgid = current->tgid;
	}

	ctx->pfn = pfn;
	ctx->nid = nid;
	ctx->access_count = tiered_mem_get_access_count(pfn);
	ctx->is_lru = folio ? (folio_test_lru(folio) ? 1 : 0) : 0;
	ctx->is_active = folio ? (folio_test_active(folio) ? 1 : 0) : 0;
	ctx->page_order = folio ? folio_order(folio) : 0;
	ctx->is_referenced = folio ? (folio_test_referenced(folio) ? 1 : 0) : 0;
	ctx->is_dirty = folio ? (folio_test_dirty(folio) ? 1 : 0) : 0;
	ctx->is_writeback = folio ? (folio_test_writeback(folio) ? 1 : 0) : 0;
	ctx->pid = pid;
	ctx->tgid = tgid;
	ctx->zone_free_pages = zone_free_pages;
	ctx->node_total_pages = node_total_pages;
}

static void tiered_pebs_overflow_handler(struct perf_event *event,
					 struct perf_sample_data *data,
					 struct pt_regs *regs)
{
	unsigned long pfn = 0;

	/* Prevent event throttling */
	event->hw.interrupts = 0;

	if (!tiered_mem_enabled)
		return;

	if (atomic_inc_return(&interval_samples_count) > samples_per_interval) {
		perf_event_disable_inatomic(event);
		return;
	}

	total_pebs_samples++;

	if (data->sample_flags & PERF_SAMPLE_PHYS_ADDR) {
		pfn = data->phys_addr >> PAGE_SHIFT;
	}

	if (!pfn && (data->sample_flags & PERF_SAMPLE_ADDR) && data->addr) {
		struct mm_struct *mm = current->mm;
		if (mm) {
			pfn = virt_to_pfn_lockless(mm, data->addr);
		}
		if (!pfn && data->addr >= PAGE_OFFSET) {
			pfn = __pa(data->addr) >> PAGE_SHIFT;
		}
	}

	if (!pfn && (data->sample_flags & PERF_SAMPLE_IP) && data->ip) {
		struct mm_struct *mm = current->mm;
		if (mm) {
			pfn = virt_to_pfn_lockless(mm, data->ip);
		}
		if (!pfn && data->ip >= PAGE_OFFSET) {
			pfn = __pa(data->ip) >> PAGE_SHIFT;
		}
	}

	if (!pfn && regs && instruction_pointer(regs)) {
		unsigned long ip = instruction_pointer(regs);
		struct mm_struct *mm = current->mm;
		if (mm) {
			pfn = virt_to_pfn_lockless(mm, ip);
		}
		if (!pfn && ip >= PAGE_OFFSET) {
			pfn = __pa(ip) >> PAGE_SHIFT;
		}
	}

	if (pfn && pfn_valid(pfn)) {
		struct tiered_mem_ops *ops;
		u64 phys = (pfn << PAGE_SHIFT);
		u64 latency = data->weight.full;
		u64 dsrc = data->data_src.val;

		/* Debug print for PEBS sample with PID and enable filtering */
		if (hook4_debug_enable && (target_pid == 0 || current->pid == target_pid || current->tgid == target_pid)) {
			printk_ratelimited(KERN_INFO "tiered_mem:HOOK4: PID=%d TGID=%d COMM=%s vaddr=0x%llx phys=0x%llx PFN=%lu lat=%llu dsrc=0x%llx\n",
					   current->pid, current->tgid, current->comm,
					   data->addr, phys, pfn,
					   latency, dsrc);
		}

		rcu_read_lock();
		ops = rcu_dereference(active_tiered_ops);
		if (ops && ops->track_access) {
			struct tiered_mem_ebpf_ctx ctx;
			populate_ebpf_ctx(&ctx, pfn);
			ops->track_access(&ctx);
			pr_info("tiered_mem:track_access: PID=%d TGID=%d COMM=%s vaddr=0x%llx phys=0x%llx PFN=%lu lat=%llu dsrc=0x%llx",
					current->pid, current->tgid, current->comm,
					data->addr, phys, pfn,
					latency, dsrc);
		} else if (tiered_page_counters && pfn < max_pfn) {
			atomic_inc(&tiered_page_counters[pfn]);
			pr_info("tiered_mem:tiered_page_counters: PID=%d TGID=%d COMM=%s vaddr=0x%llx phys=0x%llx PFN=%lu lat=%llu dsrc=0x%llx",
					current->pid, current->tgid, current->comm,
					data->addr, phys, pfn,
					latency, dsrc);
		}
		rcu_read_unlock();
	} else {
		total_pebs_errors++;
	}
}

static void tiered_pebs_work_fn(struct work_struct *work)
{
	int cpu;

	if (!tiered_mem_enabled)
		return;

	atomic_set(&interval_samples_count, 0);

	/* Re-enable events on all online CPUs */
	for_each_online_cpu(cpu) {
		struct perf_event *event = per_cpu(tiered_pebs_events, cpu);
		if (event) {
			perf_event_enable(event);
		}
	}

	schedule_delayed_work(&tiered_mem_pebs_work,
			      msecs_to_jiffies(sampling_interval));
}

DECLARE_DELAYED_WORK(tiered_mem_pebs_work, tiered_pebs_work_fn);

int tiered_pebs_init(void)
{
	int cpu;
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_RAW;
	attr.config = pebs_event_config;
	attr.size = sizeof(struct perf_event_attr);
	attr.sample_period = 10000;
	attr.precise_ip = 2; /* Request PEBS */
	attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR |
			   PERF_SAMPLE_PHYS_ADDR;
	attr.disabled = 1;
	attr.exclude_kernel = 1;
	attr.wakeup_events = 1;

	for_each_online_cpu(cpu) {
		struct perf_event *event;

		/* Try with precise_ip = 2 (PEBS) */
		attr.precise_ip = 2;
		attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR |
				   PERF_SAMPLE_PHYS_ADDR;
		event = perf_event_create_kernel_counter(
			&attr, cpu, NULL, tiered_pebs_overflow_handler, NULL);
		if (IS_ERR(event)) {
			/* Try with precise_ip = 1 */
			attr.precise_ip = 1;
			event = perf_event_create_kernel_counter(
				&attr, cpu, NULL, tiered_pebs_overflow_handler,
				NULL);
		}
		if (IS_ERR(event)) {
			/* Fall back to precise_ip = 0, no PHYS_ADDR support in HW sample */
			attr.precise_ip = 0;
			attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR;
			event = perf_event_create_kernel_counter(
				&attr, cpu, NULL, tiered_pebs_overflow_handler,
				NULL);
		}
		if (IS_ERR(event)) {
			pr_info("tiered_mem: PMU/PEBS hardware events not available (err=%ld).\n", PTR_ERR(event));
			tiered_pebs_cleanup();
			return 0;
		}
		per_cpu(tiered_pebs_events, cpu) = event;
	}

	if (tiered_mem_verbose)
		pr_info("tiered_mem: initialized PEBS events on all CPUs\n");
	return 0;
}

void tiered_pebs_cleanup(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct perf_event *event = per_cpu(tiered_pebs_events, cpu);
		if (event) {
			perf_event_disable(event);
			perf_event_release_kernel(event);
			per_cpu(tiered_pebs_events, cpu) = NULL;
		}
	}
}

void tiered_pebs_enable(void)
{
	int cpu;

	atomic_set(&interval_samples_count, 0);

	/* Enable hardware perf events if they exist */
	for_each_online_cpu(cpu) {
		struct perf_event *event = per_cpu(tiered_pebs_events, cpu);
		if (event) {
			perf_event_enable(event);
		}
	}

	/* Schedule hardware event re-enable work */
	schedule_delayed_work(&tiered_mem_pebs_work,
			      msecs_to_jiffies(sampling_interval));
}

void tiered_pebs_disable(void)
{
	int cpu;

	/* Cancel hardware event work and disable events */
	cancel_delayed_work_sync(&tiered_mem_pebs_work);
	for_each_possible_cpu(cpu) {
		struct perf_event *event = per_cpu(tiered_pebs_events, cpu);
		if (event) {
			perf_event_disable(event);
		}
	}
}