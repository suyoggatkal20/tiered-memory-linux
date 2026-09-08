/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/hugetlb.h>
#include "internal.h"
#include "../internal.h"

bool node_has_available_memory(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	unsigned long free_pages = 0;
	unsigned long min_pages = 0;
	int i;

	for (i = 0; i < MAX_NR_ZONES; i++) {
		struct zone *zone = &pgdat->node_zones[i];
		if (!populated_zone(zone))
			continue;
		free_pages += zone_page_state(zone, NR_FREE_PAGES);
		min_pages += min_wmark_pages(zone);
	}
	/* Return true if free memory is above minimum watermark + safety margin of 8MB (2048 pages) */
	return free_pages > min_pages + 2048;
}

int get_promotion_target_node(int source_nid)
{
	int nid;

	for_each_node_mask(nid, dram_nodes_mask) {
		if (node_online(nid) && node_has_available_memory(nid))
			return nid;
	}
	/* Fallback: pick first online DRAM node even if memory is tight */
	for_each_node_mask(nid, dram_nodes_mask) {
		if (node_online(nid))
			return nid;
	}
	return NUMA_NO_NODE;
}

int get_demotion_target_node(int source_nid)
{
	int nid;

	for_each_node_mask(nid, cxl_nodes_mask) {
		if (node_online(nid) && node_has_available_memory(nid))
			return nid;
	}
	/* Fallback: pick first online CXL node */
	for_each_node_mask(nid, cxl_nodes_mask) {
		if (node_online(nid))
			return nid;
	}
	return NUMA_NO_NODE;
}

static struct folio *tiered_alloc_migration_target(struct folio *src, unsigned long private)
{
	struct folio *dst = alloc_migration_target(src, private);

	if (dst) {
		unsigned long src_pfn = folio_pfn(src);
		unsigned long dst_pfn = folio_pfn(dst);

		if (tiered_page_counters && src_pfn < max_pfn && dst_pfn < max_pfn) {
			int count = atomic_xchg(&tiered_page_counters[src_pfn], 0);
			atomic_set(&tiered_page_counters[dst_pfn], count);
		}
	}
	return dst;
}

int migrate_hot_pages(struct list_head *list)
{
	struct migration_target_control mtc = {0};
	unsigned int succeeded = 0;
	int count = 0;
	struct folio *folio;
	int target_nid;

	list_for_each_entry(folio, list, lru) {
		count++;
	}

	if (count == 0)
		return 0;

	target_nid = get_promotion_target_node(NUMA_NO_NODE);
	if (target_nid == NUMA_NO_NODE) {
		target_node_unavailable_count += count;
		putback_movable_pages(list);
		return 0;
	}

	mtc.nid = target_nid;
	mtc.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE;
	mtc.reason = MR_SYSCALL;

	lru_cache_disable();
	migrate_pages(list, tiered_alloc_migration_target, NULL, (unsigned long)&mtc,
		      MIGRATE_SYNC, MR_SYSCALL, &succeeded);
	lru_cache_enable();

	total_promotions += succeeded;
	if (count > succeeded) {
		promotion_failures += (count - succeeded);
	}

	if (!list_empty(list)) {
		putback_movable_pages(list);
	}

	return succeeded;
}

int migrate_cold_pages(struct list_head *list)
{
	struct migration_target_control mtc = {0};
	unsigned int succeeded = 0;
	int count = 0;
	struct folio *folio;
	int target_nid;

	list_for_each_entry(folio, list, lru) {
		count++;
	}

	if (count == 0)
		return 0;

	target_nid = get_demotion_target_node(NUMA_NO_NODE);
	if (target_nid == NUMA_NO_NODE) {
		target_node_unavailable_count += count;
		putback_movable_pages(list);
		return 0;
	}

	mtc.nid = target_nid;
	mtc.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE;
	mtc.reason = MR_SYSCALL;

	lru_cache_disable();
	migrate_pages(list, tiered_alloc_migration_target, NULL, (unsigned long)&mtc,
		      MIGRATE_SYNC, MR_SYSCALL, &succeeded);
	lru_cache_enable();

	total_demotions += succeeded;
	if (count > succeeded) {
		demotion_failures += (count - succeeded);
	}

	if (!list_empty(list)) {
		putback_movable_pages(list);
	}

	return succeeded;
}
