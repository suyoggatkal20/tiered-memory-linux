/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_TIERED_MEM_INTERNAL_H
#define _MM_TIERED_MEM_INTERNAL_H

#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/perf_event.h>
#include <linux/workqueue.h>

#include <linux/bpf.h>

/* Global config variables */
extern bool tiered_mem_enabled;
extern bool tiered_mem_verbose;
extern nodemask_t dram_nodes_mask;
extern nodemask_t cxl_nodes_mask;

extern unsigned int sampling_interval;
extern unsigned int samples_per_interval;
extern unsigned int ageing_interval;
extern unsigned int ageing_factor;
extern unsigned int ktierd_interval;
extern unsigned int promotion_batch;
extern unsigned int demotion_batch;
extern unsigned int max_scan;
extern u64 pebs_event_config;
extern bool ageing_enabled;
extern bool ktierd_enabled;

/* Statistics */
extern u64 total_pebs_samples;
extern u64 total_pebs_errors;
extern u64 ageing_runs;
extern u64 ktierd_loops;
extern u64 total_promotions;
extern u64 total_demotions;
extern u64 promotion_failures;
extern u64 demotion_failures;
extern u64 target_node_unavailable_count;
extern u64 safety_check_failures;

/* Thresholds for hot/cold classification */
extern unsigned int hot_threshold;
extern unsigned int cold_threshold;

extern struct mutex tiered_ebpf_mutex;

struct tiered_mem_ops {
	int (*init)(struct tiered_mem_ops *ops);
	void (*track_access)(struct tiered_mem_ebpf_ctx *ctx);
	void (*age_page)(unsigned long pfn);
	int (*classify_page)(struct tiered_mem_ebpf_ctx *ctx);
	int (*get_hot_pages)(int page_count, struct list_head *list);
	int (*get_cold_pages)(int page_count, struct list_head *list);
	char name[16];
	struct module *owner;
};

extern struct tiered_mem_ops __rcu *active_tiered_ops;
int tiered_struct_ops_init(void);

void *bpf_tiered_mem_create_counters(void);
int bpf_tiered_mem_inc_counter_array(void *arr_ptr, unsigned long pfn);
int bpf_tiered_mem_get_counter_array(void *arr_ptr, unsigned long pfn);
int tiered_mem_get_access_count(unsigned long pfn);

extern struct mutex tiered_mem_config_mutex;

/* Policy structure */
struct tiered_mem_policy {
	const char *name;
	int (*get_hot_pages)(int page_count, struct list_head *list);
	int (*get_cold_pages)(int page_count, struct list_head *list);
	struct list_head list;
};

extern struct tiered_mem_policy *active_policy;

int tiered_mem_register_policy(struct tiered_mem_policy *policy);
void tiered_mem_unregister_policy(struct tiered_mem_policy *policy);
int tiered_mem_set_policy(const char *name);
int tiered_policy_init(void);
void tiered_policy_cleanup(void);

/* PEBS subsystem */
int tiered_pebs_init(void);
void tiered_pebs_cleanup(void);
void tiered_pebs_enable(void);
void tiered_pebs_disable(void);
extern struct delayed_work tiered_mem_pebs_work;

/* Ageing subsystem */
void tiered_ageing_init(void);
void tiered_ageing_cleanup(void);
extern struct delayed_work tiered_mem_ageing_work;

/* Migration engine */
int migrate_hot_pages(struct list_head *list);
int migrate_cold_pages(struct list_head *list);
int get_promotion_target_node(int source_nid);
int get_demotion_target_node(int source_nid);
bool node_has_available_memory(int nid);

/* Debugfs interface */
int tiered_debugfs_init(void);
void tiered_debugfs_cleanup(void);

#endif /* _MM_TIERED_MEM_INTERNAL_H */
