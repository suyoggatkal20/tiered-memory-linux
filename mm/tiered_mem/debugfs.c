/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/nodemask.h>
#include <linux/sched.h>
#include <linux/mmzone.h>
#include "internal.h"

static int stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "Tiered Memory Framework Status:\n");
	seq_printf(m, "  Enabled: %d\n", tiered_mem_enabled);
	seq_printf(m, "  Verbose: %d\n", tiered_mem_verbose);
	seq_printf(m, "  Active Policy: %s\n", active_policy ? active_policy->name : "none");
	seq_printf(m, "  DRAM Nodes: %*pbl\n", nodemask_pr_args(&dram_nodes_mask));
	seq_printf(m, "  CXL Nodes: %*pbl\n", nodemask_pr_args(&cxl_nodes_mask));

	seq_printf(m, "\nPEBS Statistics Collector:\n");
	seq_printf(m, "  Sampling Interval: %u ms\n", sampling_interval);
	seq_printf(m, "  Samples per Interval: %u\n", samples_per_interval);
	seq_printf(m, "  Total PEBS Samples: %llu\n", total_pebs_samples);
	seq_printf(m, "  Total PEBS Errors: %llu\n", total_pebs_errors);

	seq_printf(m, "\nAgeing:\n");
	seq_printf(m, "  Ageing Enabled: %d\n", ageing_enabled);
	seq_printf(m, "  Ageing Interval: %u ms\n", ageing_interval);
	seq_printf(m, "  Ageing Factor: %u\n", ageing_factor);
	seq_printf(m, "  Ageing Runs: %llu\n", ageing_runs);

	seq_printf(m, "\nktierd Daemon Activity:\n");
	seq_printf(m, "  ktierd Interval: %u ms\n", ktierd_interval);
	seq_printf(m, "  Promotion Batch: %u\n", promotion_batch);
	seq_printf(m, "  Demotion Batch: %u\n", demotion_batch);
	seq_printf(m, "  ktierd Loops: %llu\n", ktierd_loops);

	seq_printf(m, "\nMigration Statistics:\n");
	seq_printf(m, "  Total Promotions: %llu\n", total_promotions);
	seq_printf(m, "  Total Demotions: %llu\n", total_demotions);
	seq_printf(m, "  Promotion Failures: %llu\n", promotion_failures);
	seq_printf(m, "  Demotion Failures: %llu\n", demotion_failures);
	seq_printf(m, "  Target Node Unavailable: %llu\n", target_node_unavailable_count);
	seq_printf(m, "  Safety Check Failures: %llu\n", safety_check_failures);

	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, stats_show, NULL);
}

static const struct file_operations stats_fops = {
	.owner = THIS_MODULE,
	.open = stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int page_stats_show(struct seq_file *m, void *v)
{
	unsigned long pfn;

	seq_printf(m, "# PFN, Node, AccessCount\n");
	for (pfn = 0; pfn < max_pfn; pfn++) {
		struct page *page;

		if ((pfn & 0xffff) == 0)
			cond_resched();

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_online_page(pfn);
		if (!page)
			continue;

		int val = 0;
		if (tiered_page_counters)
			val = atomic_read(&tiered_page_counters[pfn]);
		if (val > 0) {
			seq_printf(m, "%lu, %d, %d\n", pfn, page_to_nid(page), val);
		}
	}
	return 0;
}

static int page_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, page_stats_show, NULL);
}

static const struct file_operations page_stats_fops = {
	.owner = THIS_MODULE,
	.open = page_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct dentry *debugfs_dir;

int tiered_debugfs_init(void)
{
	debugfs_dir = debugfs_create_dir("tiered_memory", NULL);
	if (!debugfs_dir)
		return -ENOMEM;

	debugfs_create_file("stats", 0444, debugfs_dir, NULL, &stats_fops);
	debugfs_create_file("page_stats", 0444, debugfs_dir, NULL, &page_stats_fops);

	return 0;
}

void tiered_debugfs_cleanup(void)
{
	debugfs_remove_recursive(debugfs_dir);
}
