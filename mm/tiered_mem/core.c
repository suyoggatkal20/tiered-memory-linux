/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/nodemask.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include "internal.h"

atomic_t *tiered_page_counters;
EXPORT_SYMBOL_GPL(tiered_page_counters);

/* Global configuration variables */
bool tiered_mem_enabled = false;
bool tiered_mem_verbose = false;
nodemask_t dram_nodes_mask = NODE_MASK_NONE;
nodemask_t cxl_nodes_mask = NODE_MASK_NONE;

unsigned int sampling_interval = 1000;
unsigned int samples_per_interval = 5000;
unsigned int ageing_interval = 10000;
unsigned int ageing_factor = 50;
unsigned int ktierd_interval = 2000;
unsigned int promotion_batch = 128;
unsigned int demotion_batch = 128;
u64 pebs_event_config = 0x20d1; /* MEM_TRANS_RETIRED.LATENCY_ABOVE_THRESHOLD */
bool ageing_enabled = true;
bool ktierd_enabled = true;

/* Thresholds for hot/cold classification */
unsigned int hot_threshold = 10;
unsigned int cold_threshold = 1;

/* Statistics */
u64 total_pebs_samples = 0;
u64 total_pebs_errors = 0;
u64 ageing_runs = 0;
u64 ktierd_loops = 0;
u64 total_promotions = 0;
u64 total_demotions = 0;
u64 promotion_failures = 0;
u64 demotion_failures = 0;
u64 target_node_unavailable_count = 0;
u64 safety_check_failures = 0;

DEFINE_MUTEX(tiered_mem_config_mutex);

static struct task_struct *ktierd_task;
static struct kobject *tiered_mem_kobj;

static void ktierd_run_iteration(void)
{
	LIST_HEAD(promotion_list);
	LIST_HEAD(demotion_list);
	int promoted = 0;
	int demoted = 0;

	if (!active_policy)
		return;

	/* 1. Promotions (CXL -> DRAM) */
	if (!nodes_empty(cxl_nodes_mask) && !nodes_empty(dram_nodes_mask)) {
		int count = active_policy->get_hot_pages(promotion_batch, &promotion_list);
		if (count > 0) {
			promoted = migrate_hot_pages(&promotion_list);
		}
	}

	/* 2. Demotions (DRAM -> CXL) */
	if (!nodes_empty(cxl_nodes_mask) && !nodes_empty(dram_nodes_mask)) {
		int count = active_policy->get_cold_pages(demotion_batch, &demotion_list);
		if (count > 0) {
			demoted = migrate_cold_pages(&demotion_list);
		}
	}

	if (tiered_mem_verbose && (promoted > 0 || demoted > 0)) {
		pr_info("tiered_mem: ktierd: promoted %d, demoted %d pages\n", promoted, demoted);
	}
}

static int ktierd_fn(void *data)
{
	pr_info("tiered_mem: ktierd background thread started\n");
	while (!kthread_should_stop()) {
		if (tiered_mem_enabled && ktierd_enabled) {
			ktierd_loops++;
			ktierd_run_iteration();
		}

		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(msecs_to_jiffies(ktierd_interval));
	}
	pr_info("tiered_mem: ktierd background thread stopping\n");
	return 0;
}

static int tiered_mem_enable(void)
{
	int err = 0;
	atomic_t *counters;

	mutex_lock(&tiered_mem_config_mutex);
	if (tiered_mem_enabled) {
		mutex_unlock(&tiered_mem_config_mutex);
		return 0;
	}

	counters = kvcalloc(max_pfn, sizeof(atomic_t), GFP_KERNEL);
	if (!counters) {
		mutex_unlock(&tiered_mem_config_mutex);
		return -ENOMEM;
	}

	err = tiered_pebs_init();
	if (err) {
		kvfree(counters);
		mutex_unlock(&tiered_mem_config_mutex);
		return err;
	}

	ktierd_task = kthread_run(ktierd_fn, NULL, "ktierd");
	if (IS_ERR(ktierd_task)) {
		err = PTR_ERR(ktierd_task);
		ktierd_task = NULL;
		tiered_pebs_cleanup();
		kvfree(counters);
		mutex_unlock(&tiered_mem_config_mutex);
		return err;
	}

	rcu_assign_pointer(tiered_page_counters, counters);

	tiered_pebs_enable();
	schedule_delayed_work(&tiered_mem_ageing_work, msecs_to_jiffies(ageing_interval));

	tiered_mem_enabled = true;
	mutex_unlock(&tiered_mem_config_mutex);

	pr_info("tiered_mem: Modular Tiered Memory Framework enabled\n");
	return 0;
}

static void tiered_mem_disable(void)
{
	atomic_t *old_counters;

	mutex_lock(&tiered_mem_config_mutex);
	if (!tiered_mem_enabled) {
		mutex_unlock(&tiered_mem_config_mutex);
		return;
	}

	tiered_mem_enabled = false;

	if (ktierd_task) {
		kthread_stop(ktierd_task);
		ktierd_task = NULL;
	}

	cancel_delayed_work_sync(&tiered_mem_ageing_work);
	tiered_pebs_disable();
	tiered_pebs_cleanup();

	old_counters = tiered_page_counters;
	rcu_assign_pointer(tiered_page_counters, NULL);

	mutex_unlock(&tiered_mem_config_mutex);

	synchronize_rcu();
	kvfree(old_counters);

	pr_info("tiered_mem: Modular Tiered Memory Framework disabled\n");
}

/* sysfs interfaces */
static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", tiered_mem_enabled);
}

static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val, err;

	err = kstrtoint(buf, 10, &val);
	if (err)
		return err;

	if (val == 1) {
		err = tiered_mem_enable();
		if (err)
			return err;
	} else if (val == 0) {
		tiered_mem_disable();
	} else {
		return -EINVAL;
	}
	return count;
}
static struct kobj_attribute enable_attribute = __ATTR_RW(enable);

static ssize_t dram_nodes_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&dram_nodes_mask));
}

static ssize_t dram_nodes_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	nodemask_t new_mask;
	int err;

	err = nodelist_parse(buf, new_mask);
	if (err)
		return err;

	mutex_lock(&tiered_mem_config_mutex);
	dram_nodes_mask = new_mask;
	mutex_unlock(&tiered_mem_config_mutex);
	return count;
}
static struct kobj_attribute dram_nodes_attribute = __ATTR_RW(dram_nodes);

static ssize_t cxl_nodes_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&cxl_nodes_mask));
}

static ssize_t cxl_nodes_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	nodemask_t new_mask;
	int err;

	err = nodelist_parse(buf, new_mask);
	if (err)
		return err;

	mutex_lock(&tiered_mem_config_mutex);
	cxl_nodes_mask = new_mask;
	mutex_unlock(&tiered_mem_config_mutex);
	return count;
}
static struct kobj_attribute cxl_nodes_attribute = __ATTR_RW(cxl_nodes);

#define TIERED_ATTR_RW_UINT(name, min_val, max_val) \
static ssize_t name##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
{ \
	return sysfs_emit(buf, "%u\n", name); \
} \
static ssize_t name##_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) \
{ \
	unsigned int val; \
	int err = kstrtouint(buf, 10, &val); \
	if (err) \
		return err; \
	if (val < (min_val) || val > (max_val)) \
		return -EINVAL; \
	name = val; \
	return count; \
} \
static struct kobj_attribute name##_attribute = __ATTR_RW(name);

TIERED_ATTR_RW_UINT(sampling_interval, 10, UINT_MAX)
TIERED_ATTR_RW_UINT(samples_per_interval, 10, UINT_MAX)
TIERED_ATTR_RW_UINT(ageing_interval, 10, UINT_MAX)
TIERED_ATTR_RW_UINT(ageing_factor, 0, 100)
TIERED_ATTR_RW_UINT(ktierd_interval, 10, UINT_MAX)
TIERED_ATTR_RW_UINT(promotion_batch, 1, 10000)
TIERED_ATTR_RW_UINT(demotion_batch, 1, 10000)
TIERED_ATTR_RW_UINT(hot_threshold, 0, UINT_MAX)
TIERED_ATTR_RW_UINT(cold_threshold, 0, UINT_MAX)

static ssize_t verbose_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", tiered_mem_verbose);
}
static ssize_t verbose_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool val;
	int err = kstrtobool(buf, &val);
	if (err)
		return err;
	tiered_mem_verbose = val;
	return count;
}
static struct kobj_attribute verbose_attribute = __ATTR_RW(verbose);

static ssize_t ageing_enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ageing_enabled);
}
static ssize_t ageing_enabled_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool val;
	int err = kstrtobool(buf, &val);
	if (err)
		return err;
	ageing_enabled = val;
	return count;
}
static struct kobj_attribute ageing_enabled_attribute = __ATTR_RW(ageing_enabled);

static ssize_t ktierd_enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", ktierd_enabled);
}
static ssize_t ktierd_enabled_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	bool val;
	int err = kstrtobool(buf, &val);
	if (err)
		return err;
	ktierd_enabled = val;
	return count;
}
static struct kobj_attribute ktierd_enabled_attribute = __ATTR_RW(ktierd_enabled);

static ssize_t pebs_event_config_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0x%llx\n", pebs_event_config);
}
static ssize_t pebs_event_config_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	u64 val;
	int err = kstrtoull(buf, 16, &val);
	if (err)
		return err;
	pebs_event_config = val;
	return count;
}
static struct kobj_attribute pebs_event_config_attribute = __ATTR_RW(pebs_event_config);

static ssize_t policy_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", active_policy ? active_policy->name : "none");
}
static ssize_t policy_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char name[32];
	int err;
	size_t len = min(count, sizeof(name) - 1);

	memcpy(name, buf, len);
	name[len] = '\0';
	if (len > 0 && name[len - 1] == '\n')
		name[len - 1] = '\0';

	err = tiered_mem_set_policy(name);
	if (err)
		return err;

	return count;
}
static struct kobj_attribute policy_attribute = __ATTR_RW(policy);

static struct attribute *tiered_mem_attrs[] = {
	&enable_attribute.attr,
	&dram_nodes_attribute.attr,
	&cxl_nodes_attribute.attr,
	&sampling_interval_attribute.attr,
	&samples_per_interval_attribute.attr,
	&ageing_interval_attribute.attr,
	&ageing_factor_attribute.attr,
	&ktierd_interval_attribute.attr,
	&promotion_batch_attribute.attr,
	&demotion_batch_attribute.attr,
	&hot_threshold_attribute.attr,
	&cold_threshold_attribute.attr,
	&verbose_attribute.attr,
	&ageing_enabled_attribute.attr,
	&ktierd_enabled_attribute.attr,
	&pebs_event_config_attribute.attr,
	&policy_attribute.attr,
	NULL,
};

static struct attribute_group tiered_mem_attr_group = {
	.attrs = tiered_mem_attrs,
};

static int __init tiered_mem_init(void)
{
	int err;

	tiered_mem_kobj = kobject_create_and_add("tiered_memory", kernel_kobj);
	if (!tiered_mem_kobj)
		return -ENOMEM;

	err = sysfs_create_group(tiered_mem_kobj, &tiered_mem_attr_group);
	if (err) {
		kobject_put(tiered_mem_kobj);
		return err;
	}

	err = tiered_policy_init();
	if (err) {
		sysfs_remove_group(tiered_mem_kobj, &tiered_mem_attr_group);
		kobject_put(tiered_mem_kobj);
		return err;
	}

	err = tiered_debugfs_init();
	if (err) {
		tiered_policy_cleanup();
		sysfs_remove_group(tiered_mem_kobj, &tiered_mem_attr_group);
		kobject_put(tiered_mem_kobj);
		return err;
	}

	pr_info("tiered_mem: initialized Modular Tiered Memory Framework\n");
	return 0;
}

late_initcall(tiered_mem_init);
