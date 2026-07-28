/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/mmzone.h>
#include "internal.h"

static void tiered_ageing_work_fn(struct work_struct *work)
{
	unsigned long pfn;

	if (!tiered_mem_enabled || !ageing_enabled)
		return;

	ageing_runs++;

	for (pfn = 0; pfn < max_pfn; pfn++) {
		struct page *page;
		int old, new_val;

		if ((pfn & 0xff) == 0)
			cond_resched();

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_online_page(pfn);
		if (!page)
			continue;

		if (tiered_page_counters) {
			old = atomic_read(&tiered_page_counters[pfn]);
			if (old > 0) {
				new_val = (old * ageing_factor) / 100;
				while (atomic_cmpxchg(&tiered_page_counters[pfn], old, new_val) != old) {
					old = atomic_read(&tiered_page_counters[pfn]);
					new_val = (old * ageing_factor) / 100;
				}
			}
		}
	}

	schedule_delayed_work(&tiered_mem_ageing_work, msecs_to_jiffies(ageing_interval));
}

DECLARE_DELAYED_WORK(tiered_mem_ageing_work, tiered_ageing_work_fn);

void tiered_ageing_init(void)
{
	if (tiered_mem_verbose)
		pr_info("tiered_mem: initialized ageing subsystem\n");
}

void tiered_ageing_cleanup(void)
{
	cancel_delayed_work_sync(&tiered_mem_ageing_work);
}
