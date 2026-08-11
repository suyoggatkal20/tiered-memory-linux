
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/security.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include "internal.h"

BPF_CALL_1(bpf_tiered_mem_node_free_pages, int, nid)
{
	pg_data_t *pgdat;
	unsigned long free_pages = 0;
	int i;

	if (nid < 0 || nid >= MAX_NUMNODES || !node_online(nid))
		return 0;

	pgdat = NODE_DATA(nid);
	for (i = 0; i < MAX_NR_ZONES; i++) {
		struct zone *zone = &pgdat->node_zones[i];
		if (populated_zone(zone))
			free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}
	return (u64)free_pages;
}

static const struct bpf_func_proto bpf_tiered_mem_node_free_pages_proto = {
	.func		= bpf_tiered_mem_node_free_pages,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
};

BPF_CALL_0(bpf_tiered_mem_get_hot_threshold)
{
	return (u64)hot_threshold;
}

static const struct bpf_func_proto bpf_tiered_mem_get_hot_threshold_proto = {
	.func		= bpf_tiered_mem_get_hot_threshold,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
};

BPF_CALL_0(bpf_tiered_mem_get_cold_threshold)
{
	return (u64)cold_threshold;
}

static const struct bpf_func_proto bpf_tiered_mem_get_cold_threshold_proto = {
	.func		= bpf_tiered_mem_get_cold_threshold,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
};

BPF_CALL_1(bpf_tiered_mem_is_dram_node, int, nid)
{
	if (nid < 0 || nid >= MAX_NUMNODES)
		return 0;
	return node_isset(nid, dram_nodes_mask) ? 1 : 0;
}

static const struct bpf_func_proto bpf_tiered_mem_is_dram_node_proto = {
	.func		= bpf_tiered_mem_is_dram_node,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
};

BPF_CALL_1(bpf_tiered_mem_is_cxl_node, int, nid)
{
	if (nid < 0 || nid >= MAX_NUMNODES)
		return 0;
	return node_isset(nid, cxl_nodes_mask) ? 1 : 0;
}

static const struct bpf_func_proto bpf_tiered_mem_is_cxl_node_proto = {
	.func		= bpf_tiered_mem_is_cxl_node,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
};

atomic_t *active_custom_counters = NULL;
EXPORT_SYMBOL_GPL(active_custom_counters);

BPF_CALL_0(bpf_tiered_mem_get_page_counters)
{
	atomic_t *arr;

	if (!max_pfn)
		return 0;

	arr = kvcalloc(max_pfn, sizeof(atomic_t), GFP_KERNEL | __GFP_NOWARN);
	active_custom_counters = arr;
	return (u64)(unsigned long)arr;
}

static const struct bpf_func_proto bpf_tiered_mem_get_page_counters_proto = {
	.func		= bpf_tiered_mem_get_page_counters,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
};

static unsigned long normalize_pfn(unsigned long pfn)
{
	if (!max_pfn)
		return 0;

	if (pfn < max_pfn)
		return pfn;

	if (pfn >= PAGE_OFFSET)
		pfn = __pa(pfn) >> PAGE_SHIFT;
	else
		pfn = pfn >> PAGE_SHIFT;

	if (pfn >= max_pfn)
		pfn = pfn % max_pfn;

	return pfn;
}

BPF_CALL_2(bpf_tiered_mem_inc_page_counter, atomic_t *, arr, unsigned long, raw_pfn)
{
	unsigned long pfn;
	int count;

	if (!arr || !max_pfn)
		return 0;

	pfn = normalize_pfn(raw_pfn);
	atomic_inc(&arr[pfn]);
	count = atomic_read(&arr[pfn]);

	trace_printk("tiered_mem: page %lu access updated in custom array to count %d\n", pfn, count);
	return count;
}

static const struct bpf_func_proto bpf_tiered_mem_inc_page_counter_proto = {
	.func		= bpf_tiered_mem_inc_page_counter,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
	.arg2_type	= ARG_ANYTHING,
};

BPF_CALL_2(bpf_tiered_mem_get_page_counter, atomic_t *, arr, unsigned long, raw_pfn)
{
	unsigned long pfn;

	if (!arr || !max_pfn)
		return 0;

	pfn = normalize_pfn(raw_pfn);
	return atomic_read(&arr[pfn]);
}

static const struct bpf_func_proto bpf_tiered_mem_get_page_counter_proto = {
	.func		= bpf_tiered_mem_get_page_counter,
	.gpl_only	= true,
	.ret_type	= RET_INTEGER,
	.arg1_type	= ARG_ANYTHING,
	.arg2_type	= ARG_ANYTHING,
};

__bpf_kfunc void *bpf_tiered_mem_create_counters(void)
{
	if (!max_pfn)
		return NULL;
	return kvcalloc(max_pfn, sizeof(atomic_t), GFP_KERNEL | __GFP_NOWARN);
}

__bpf_kfunc int bpf_tiered_mem_inc_counter_array(void *arr_ptr, unsigned long raw_pfn)
{
	atomic_t *arr = (atomic_t *)arr_ptr;
	unsigned long pfn;

	if (!arr || !max_pfn)
		return 0;

	pfn = normalize_pfn(raw_pfn);
	atomic_inc(&arr[pfn]);
	return atomic_read(&arr[pfn]);
}

__bpf_kfunc int bpf_tiered_mem_get_counter_array(void *arr_ptr, unsigned long raw_pfn)
{
	atomic_t *arr = (atomic_t *)arr_ptr;
	unsigned long pfn;

	if (!arr || !max_pfn)
		return 0;

	pfn = normalize_pfn(raw_pfn);
	return atomic_read(&arr[pfn]);
}

int tiered_mem_get_access_count(unsigned long raw_pfn)
{
	unsigned long pfn;

	if (!max_pfn)
		return 0;

	pfn = normalize_pfn(raw_pfn);

	if (active_custom_counters)
		return atomic_read(&active_custom_counters[pfn]);
	if (tiered_page_counters)
		return atomic_read(&tiered_page_counters[pfn]);

	return 0;
}
EXPORT_SYMBOL_GPL(tiered_mem_get_access_count);

static const struct bpf_func_proto *
tiered_mem_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch ((int)func_id) {
	case BPF_FUNC_get_numa_node_id:
		return &bpf_get_numa_node_id_proto;

	case BPF_FUNC_tiered_mem_create_page_counters:
		return &bpf_tiered_mem_get_page_counters_proto;
	case BPF_FUNC_tiered_mem_inc_page_counter:
		return &bpf_tiered_mem_inc_page_counter_proto;
	case BPF_FUNC_tiered_mem_get_page_counter:
		return &bpf_tiered_mem_get_page_counter_proto;

	case BPF_FUNC_map_lookup_elem:
		return &bpf_map_lookup_elem_proto;
	case BPF_FUNC_map_update_elem:
		return &bpf_map_update_elem_proto;
	case BPF_FUNC_map_delete_elem:
		return &bpf_map_delete_elem_proto;

	case BPF_FUNC_trace_printk:
		return bpf_get_trace_printk_proto();
	case BPF_FUNC_ktime_get_ns:
		return &bpf_ktime_get_ns_proto;

	default:
		return bpf_base_func_proto(func_id, prog);
	}
}

static bool tiered_mem_is_valid_access(int off, int size,
				       enum bpf_access_type type,
				       const struct bpf_prog *prog,
				       struct bpf_insn_access_aux *info)
{
	if (type == BPF_WRITE)
		return false;

	if (off < 0 || off + size > sizeof(struct tiered_mem_ebpf_ctx))
		return false;

	if (off % size != 0)
		return false;

	return true;
}

const struct bpf_verifier_ops tiered_mem_verifier_ops = {
	.get_func_proto  = tiered_mem_func_proto,
	.is_valid_access = tiered_mem_is_valid_access,
};

const struct bpf_prog_ops tiered_mem_prog_ops = {
};

struct tiered_mem_ops __rcu *active_tiered_ops = NULL;
EXPORT_SYMBOL_GPL(active_tiered_ops);

static int default_bpf_init(struct tiered_mem_ops *ops)
{
	return 0;
}

static void default_bpf_track_access(unsigned long pfn)
{
}

static int default_bpf_get_hot_pages(int page_count, struct list_head *list)
{
	return 0;
}

static int default_bpf_get_cold_pages(int page_count, struct list_head *list)
{
	return 0;
}

static struct tiered_mem_ops __bpf_tiered_mem_ops = {
	.init = default_bpf_init,
	.track_access = default_bpf_track_access,
	.get_hot_pages = default_bpf_get_hot_pages,
	.get_cold_pages = default_bpf_get_cold_pages,
};

static int bpf_tiered_mem_ops_reg(void *kdata, struct bpf_link *link)
{
	struct tiered_mem_ops *ops = kdata;
	struct tiered_mem_ops *old_ops;

	mutex_lock(&tiered_ebpf_mutex);
	old_ops = rcu_dereference_protected(active_tiered_ops, lockdep_is_held(&tiered_ebpf_mutex));
	if (old_ops) {
		mutex_unlock(&tiered_ebpf_mutex);
		return -EBUSY;
	}
	rcu_assign_pointer(active_tiered_ops, ops);
	mutex_unlock(&tiered_ebpf_mutex);

	if (ops->init)
		ops->init(ops);

	pr_info("tiered_mem: registered struct_ops '%s'\n", ops->name[0] ? ops->name : "unnamed");
	return 0;
}

static void bpf_tiered_mem_ops_unreg(void *kdata, struct bpf_link *link)
{
	struct tiered_mem_ops *ops = kdata;

	mutex_lock(&tiered_ebpf_mutex);
	if (rcu_access_pointer(active_tiered_ops) == ops) {
		rcu_assign_pointer(active_tiered_ops, NULL);
	}
	mutex_unlock(&tiered_ebpf_mutex);

	synchronize_rcu();
	pr_info("tiered_mem: unregistered struct_ops '%s'\n", ops->name[0] ? ops->name : "unnamed");
}

static int bpf_tiered_mem_ops_init(struct btf *btf)
{
	return 0;
}

static int bpf_tiered_mem_ops_init_member(const struct btf_type *t,
					  const struct btf_member *member,
					  void *kdata, const void *udata)
{
	const struct tiered_mem_ops *uops = udata;
	struct tiered_mem_ops *ops = kdata;
	u32 moff;

	moff = __btf_member_bit_offset(t, member) / 8;
	if (moff == offsetof(struct tiered_mem_ops, name)) {
		if (bpf_obj_name_cpy(ops->name, uops->name, sizeof(ops->name)) <= 0)
			return -EINVAL;
		return 1;
	}
	return 0;
}

static const struct bpf_verifier_ops tiered_mem_struct_ops_verifier_ops = {
	.get_func_proto  = tiered_mem_func_proto,
	.is_valid_access = tiered_mem_is_valid_access,
};

struct bpf_struct_ops bpf_tiered_mem_ops = {
	.verifier_ops = &tiered_mem_struct_ops_verifier_ops,
	.init = bpf_tiered_mem_ops_init,
	.init_member = bpf_tiered_mem_ops_init_member,
	.reg = bpf_tiered_mem_ops_reg,
	.unreg = bpf_tiered_mem_ops_unreg,
	.cfi_stubs = &__bpf_tiered_mem_ops,
	.name = "tiered_mem_ops",
	.owner = THIS_MODULE,
};

int tiered_struct_ops_init(void)
{
	return register_bpf_struct_ops(&bpf_tiered_mem_ops, tiered_mem_ops);
}
