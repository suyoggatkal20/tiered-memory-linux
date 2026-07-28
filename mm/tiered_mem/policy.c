/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include "internal.h"
#include "../internal.h"

static DEFINE_MUTEX(policy_mutex);
static LIST_HEAD(policy_list);
struct tiered_mem_policy *active_policy;

static unsigned long last_scanned_dram_pfn;
static unsigned long last_scanned_cxl_pfn;

int tiered_mem_register_policy(struct tiered_mem_policy *policy)
{
	if (!policy || !policy->name)
		return -EINVAL;

	mutex_lock(&policy_mutex);
	list_add_tail(&policy->list, &policy_list);
	if (!active_policy)
		active_policy = policy;
	mutex_unlock(&policy_mutex);

	pr_info("tiered_mem: registered policy '%s'\n", policy->name);
	return 0;
}

void tiered_mem_unregister_policy(struct tiered_mem_policy *policy)
{
	if (!policy)
		return;

	mutex_lock(&policy_mutex);
	list_del(&policy->list);
	if (active_policy == policy) {
		active_policy = list_first_entry_or_null(&policy_list,
							 struct tiered_mem_policy,
							 list);
	}
	mutex_unlock(&policy_mutex);

	pr_info("tiered_mem: unregistered policy '%s'\n", policy->name);
}

int tiered_mem_set_policy(const char *name)
{
	struct tiered_mem_policy *p;
	int found = 0;

	mutex_lock(&policy_mutex);
	list_for_each_entry(p, &policy_list, list) {
		if (strcmp(p->name, name) == 0) {
			active_policy = p;
			found = 1;
			break;
		}
	}
	mutex_unlock(&policy_mutex);

	if (found) {
		if (tiered_mem_verbose)
			pr_info("tiered_mem: set active policy to '%s'\n", name);
		return 0;
	}
	return -EINVAL;
}

static int default_get_hot_pages(int page_count, struct list_head *list)
{
	int nid;
	int isolated = 0;
	unsigned long pfn;
	unsigned long start_pfn, end_pfn;
	struct page *page;
	struct folio *folio;

	/* Need to hold configuration lock to prevent nodemask modifications */
	mutex_lock(&tiered_mem_config_mutex);

	for_each_node_mask(nid, cxl_nodes_mask) {
		pg_data_t *pgdat = NODE_DATA(nid);
		int i;

		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *zone = &pgdat->node_zones[i];
			if (!populated_zone(zone))
				continue;

			start_pfn = zone->zone_start_pfn;
			end_pfn = zone_end_pfn(zone);

			if (last_scanned_cxl_pfn >= start_pfn && last_scanned_cxl_pfn < end_pfn)
				pfn = last_scanned_cxl_pfn;
			else
				pfn = start_pfn;

			unsigned long initial_pfn = pfn;
			bool wrapped = false;

			while (isolated < page_count) {
				if ((pfn & 0xff) == 0)
					cond_resched();

				page = pfn_to_online_page(pfn);
				if (page && !PageTail(page)) {
					folio = page_folio(page);
					int access_count = tiered_page_counters ? atomic_read(&tiered_page_counters[pfn]) : 0;
					if (access_count >= hot_threshold &&
					    folio_test_lru(folio) &&
					    !folio_test_reserved(folio) &&
					    !folio_test_mlocked(folio) &&
					    !folio_test_writeback(folio) &&
					    !folio_maybe_dma_pinned(folio)) {

						if (folio_isolate_lru(folio)) {
							list_add_tail(&folio->lru, list);
							isolated++;
						} else {
							safety_check_failures++;
						}
					}
				}

				pfn++;
				if (pfn >= end_pfn) {
					pfn = start_pfn;
					wrapped = true;
				}

				if (wrapped && pfn >= initial_pfn)
					break; /* scanned the whole zone */
			}

			last_scanned_cxl_pfn = pfn;
			if (isolated >= page_count)
				break;
		}
		if (isolated >= page_count)
			break;
	}

	mutex_unlock(&tiered_mem_config_mutex);
	return isolated;
}

static int default_get_cold_pages(int page_count, struct list_head *list)
{
	int nid;
	int isolated = 0;
	unsigned long pfn;
	unsigned long start_pfn, end_pfn;
	struct page *page;
	struct folio *folio;

	mutex_lock(&tiered_mem_config_mutex);

	for_each_node_mask(nid, dram_nodes_mask) {
		pg_data_t *pgdat = NODE_DATA(nid);
		int i;

		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *zone = &pgdat->node_zones[i];
			if (!populated_zone(zone))
				continue;

			start_pfn = zone->zone_start_pfn;
			end_pfn = zone_end_pfn(zone);

			if (last_scanned_dram_pfn >= start_pfn && last_scanned_dram_pfn < end_pfn)
				pfn = last_scanned_dram_pfn;
			else
				pfn = start_pfn;

			unsigned long initial_pfn = pfn;
			bool wrapped = false;

			while (isolated < page_count) {
				if ((pfn & 0xff) == 0)
					cond_resched();

				page = pfn_to_online_page(pfn);
				if (page && !PageTail(page)) {
					folio = page_folio(page);
					int access_count = tiered_page_counters ? atomic_read(&tiered_page_counters[pfn]) : 0;
					if (access_count <= cold_threshold &&
					    folio_test_lru(folio) &&
					    !folio_test_reserved(folio) &&
					    !folio_test_mlocked(folio) &&
					    !folio_test_writeback(folio) &&
					    !folio_maybe_dma_pinned(folio)) {

						if (folio_isolate_lru(folio)) {
							list_add_tail(&folio->lru, list);
							isolated++;
						} else {
							safety_check_failures++;
						}
					}
				}

				pfn++;
				if (pfn >= end_pfn) {
					pfn = start_pfn;
					wrapped = true;
				}

				if (wrapped && pfn >= initial_pfn)
					break;
			}

			last_scanned_dram_pfn = pfn;
			if (isolated >= page_count)
				break;
		}
		if (isolated >= page_count)
			break;
	}

	mutex_unlock(&tiered_mem_config_mutex);
	return isolated;
}

static struct tiered_mem_policy default_policy = {
	.name = "default",
	.get_hot_pages = default_get_hot_pages,
	.get_cold_pages = default_get_cold_pages,
};

int __init tiered_policy_init(void)
{
	return tiered_mem_register_policy(&default_policy);
}

void tiered_policy_cleanup(void)
{
	tiered_mem_unregister_policy(&default_policy);
}
