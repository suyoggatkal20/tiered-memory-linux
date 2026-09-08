/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/security.h>
#include "internal.h"

static bool tiered_mem_is_valid_access(int off, int size, enum bpf_access_type type,
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
static const struct bpf_func_proto *tiered_mem_get_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_trace_printk:
		return bpf_get_trace_printk_proto();
	default:
		return bpf_base_func_proto(func_id, prog);
	}
}

const struct bpf_verifier_ops tiered_mem_verifier_ops = {
	.get_func_proto  = tiered_mem_get_func_proto,
	.is_valid_access = tiered_mem_is_valid_access,
};

const struct bpf_prog_ops tiered_mem_prog_ops = {
	
};
