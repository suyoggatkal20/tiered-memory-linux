# Tiered Memory (eBPF struct_ops) — Progress Log

Branch: `feature/struct_ops`
Last updated: 2026-08-22

## What this project is

A Linux kernel patch set (`mm/tiered_mem/`) that adds a DRAM/CXL tiered
memory subsystem. Page hotness is sampled via Intel PEBS (or perf's
generic sampling path), tracked with per-PFN counters, and promotion/
demotion decisions are delegated to a pluggable eBPF policy loaded
through a custom `bpf_struct_ops` type (`struct tiered_mem_ops`), so the
hot/cold classification logic can be swapped without rebuilding the
kernel.

## Commit history so far

1. **`51dbde8d` – ebpf implemented**
   First working version: `tiered_mem_ebpf_ctx`, the 4 core BPF helpers
   (create/inc/get/decay page counters, IDs 212–215), and a default
   `tiered_struct_ops.bpf.c` policy (promote at count ≥10, demote at
   count <3).

2. **`b496ccda` – added struct ops**
   Wired up `bpf_struct_ops` registration/lifecycle (`policy_ebpf.c`,
   `Makefile.bpf`, `loader.c`), so a compiled `.bpf.o` can be registered
   via `bpftool struct_ops register` and hooked into the PEBS/ageing/
   scan paths instead of the hardcoded default policy.

3. **`b25b61a6`** — housekeeping (`.gitignore` for the rootfs image).

4. **`71b3e5af` – removed tier mem hook**
   Ripped out the old software page-table sampler fallback
   (`use_software_sampler`, `software_sampler_work`) and a leftover
   experimental hook; simplified `pebs.c`/`policy.c`/`ebpf.c` to rely on
   hardware PEBS only, and reworked `qemu_setup/setup_configs.sh`.

## Current work in progress (uncommitted)

Focus: **per-PID debug tracing + hardening the PFN resolution path.**

- **`kernel/events/core.c`** — new "Hook 5" debug print in
  `perf_prepare_sample()`: logs PID/TGID/comm/vaddr/phys/PFN/latency/
  data-source for every perf sample, gated by `hook4_debug_enable` and
  an optional `target_pid` filter.
- **`mm/tiered_mem/pebs.c`**
  - `virt_to_pfn_lockless()` rewritten to correctly detect PUD/PMD huge
    leaf entries (1GB/2MB pages) instead of misreading them as
    next-level table pointers, and now uses `READ_ONCE()` at each level
    since it runs in NMI/PMI context.
  - Removed the rmap_walk()/anon_vma fallback that used to resolve
    owning PID from just a PFN — it needs sleeping locks, which is
    unsafe from NMI context; PID/TGID now come from `current` only.
  - `populate_ebpf_ctx()` moved out of this file (now shared, declared
    in `internal.h`) and extended to fill `pid`/`tgid`.
  - `tiered_pebs_overflow_handler()` gets the same PID-filtered
    "HOOK4" debug print as above.
- **`mm/tiered_mem/core.c`** — new sysfs knobs `target_pid` (filter
  debug output to one PID/TGID, 0 = all) and `hook4_debug_enable`
  (on/off switch for the new debug prints); removed the now-dead
  `max_scan` knob (was only used by the deleted software sampler).
- **`mm/tiered_mem/policy.c`** — `default_get_hot_pages`/
  `default_get_cold_pages` now build their `tiered_mem_ebpf_ctx` via
  the shared `populate_ebpf_ctx()` helper instead of duplicating the
  struct literal inline.
- **`mm/tiered_mem/ebpf.c`**
  - Renamed `bpf_tiered_mem_age_page_counter` → `..._decay_page_counter`
    for consistency with the public helper name.
  - Added `normalize_pfn()` (`pfn % max_pfn`) so an out-of-range PFN
    from BPF can no longer index the counter array out of bounds.
  - Added `bpf_tiered_mem_ops_init_member()` so the verifier accepts
    the `name` field being set from the user-supplied struct_ops map.
- **`mm/tiered_mem/tiered_struct_ops.bpf.c`**
  - Added `get_tiered_ctx()`, which uses `bpf_probe_read_kernel()` to
    safely copy the kernel-side ctx pointer into BPF stack memory
    instead of dereferencing kernel fields directly.
  - `track_access`/`classify_page` updated to use it and to log PID
    alongside PFN.
- **`include/uapi/linux/bpf.h`** — `pid`/`tgid` fields added to the
  UAPI `tiered_mem_ebpf_ctx` struct.
- **`qemu_setup/setup_configs.sh`** — config changes to support the
  above (details in `git diff`).
- **`run.md`** — appended the actual command sequence used this session
  to reconfigure/rebuild/reinstall the kernel (`CONFIG_LOCALVERSION`
  bump to `-tiered-btf-4`, `make olddefconfig`, `modules_install`,
  `update-grub`, `grub-reboot`) and to drive the new `target_pid` /
  `hook4_debug_enable` knobs and watch `dmesg -w | grep tiered_mem:HOOK4`.

## New, currently untracked files

- `mm/tiered_mem/test_page_access.c` — small userspace workload: mmaps
  2048 anonymous pages (8MB) and hammers them in a tight read/write
  loop forever, so PEBS has a hot, stable working set to sample against.
- `test_workload` — compiled binary of the above (built inside the VM).
- `mm/tiered_mem/README_STRUCT_OPS.md` — reference doc for the
  currently-implemented struct_ops hooks, ctx fields, and the 4 BPF
  helpers, with a full worked policy example.
- `plan.md` — forward-looking API design doc: proposed *additional*
  struct_ops hooks (`should_migrate`, `on_promote`, `on_demote`,
  `on_memory_pressure`, `select_target_node`, ...), proposed ctx fields
  (PEBS latency/data_source, page age, memory pressure, process info),
  and ~20 proposed new BPF helpers, phased into a 3-stage priority list.
  Nothing in it is implemented yet — it's the roadmap for after the
  current debug-tracing work lands.

## Likely next steps (from `plan.md` Phase 1)

- `should_migrate` hook for fine-grained per-page migration veto.
- `on_promote` / `on_demote` notification hooks (reset counters after
  a migration completes).
- `bpf_tiered_mem_get_page_nid` / `is_page_on_dram` / `is_page_on_cxl`
  helpers.
- Extend `tiered_mem_ebpf_ctx` with `data_source` and `access_latency`
  (the raw values are already being captured for the HOOK4/HOOK5 debug
  prints — next step is threading them into the ctx struct itself).
