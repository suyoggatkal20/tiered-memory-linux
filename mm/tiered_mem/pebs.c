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
static bool use_software_sampler = false;
static void tiered_software_sampler_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(software_sampler_work, tiered_software_sampler_fn);

static unsigned long virt_to_pfn_lockless(struct mm_struct *mm,
					  unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
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

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	if (pmd_leaf(*pmd) || pmd_trans_huge(*pmd)) {
		pfn = pmd_pfn(*pmd) + ((addr & ~PMD_MASK) >> PAGE_SHIFT);
		return pfn;
	}

	ptep = pte_offset_kernel(pmd, addr);
	if (ptep) {
		pte = *ptep;
		if (pte_present(pte)) {
			pfn = pte_pfn(pte);
		}
	}

	return pfn;
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
	}

	if (pfn && pfn_valid(pfn)) {
		if (tiered_page_counters && pfn < max_pfn) {
			atomic_inc(&tiered_page_counters[pfn]);
		}
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

	use_software_sampler = false;

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
			/* If hardware creation fails, clean up and set up software sampler */
			pr_warn("tiered_mem: PMU/PEBS hardware events not available (err=%ld). Falling back to Software Page-Table Sampler.\n", PTR_ERR(event));
			tiered_pebs_cleanup();
			use_software_sampler = true;
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

	if (use_software_sampler) {
		schedule_delayed_work(&software_sampler_work,
				      msecs_to_jiffies(sampling_interval));
		return;
	}

	for_each_online_cpu(cpu) {
		struct perf_event *event = per_cpu(tiered_pebs_events, cpu);
		if (event) {
			perf_event_enable(event);
		}
	}
	schedule_delayed_work(&tiered_mem_pebs_work,
			      msecs_to_jiffies(sampling_interval));
}

void tiered_pebs_disable(void)
{
	int cpu;

	if (use_software_sampler) {
		cancel_delayed_work_sync(&software_sampler_work);
		return;
	}

	cancel_delayed_work_sync(&tiered_mem_pebs_work);
	for_each_possible_cpu(cpu) {
		struct perf_event *event = per_cpu(tiered_pebs_events, cpu);
		if (event) {
			perf_event_disable(event);
		}
	}
}

static void tiered_software_sampler_fn(struct work_struct *work)
{
	unsigned long pfn;
	int scanned = 0;
	int max_scan = 10000; /* scan at most 10k pages per interval */
	static unsigned long next_pfn = 0;

	if (!tiered_mem_enabled)
		return;

	if (next_pfn >= max_pfn)
		next_pfn = 0;

	for (pfn = next_pfn; pfn < max_pfn && scanned < max_scan; pfn++) {
		struct page *page;
		struct folio *folio;
		unsigned long vm_flags = 0;

		if ((pfn & 0xff) == 0)
			cond_resched();

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_online_page(pfn);
		if (!page || PageTail(page))
			continue;

		folio = page_folio(page);
		if (!folio_test_lru(folio))
			continue;

		scanned++;
		if (folio_referenced(folio, 0, NULL, &vm_flags) > 0) {
			if (tiered_page_counters && pfn < max_pfn) {
				atomic_inc(&tiered_page_counters[pfn]);
				total_pebs_samples++;
			}
		}
	}
	next_pfn = pfn;

	schedule_delayed_work(&software_sampler_work, msecs_to_jiffies(sampling_interval));
}
