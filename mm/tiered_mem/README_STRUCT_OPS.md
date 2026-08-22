# Tiered Memory eBPF `struct_ops` & Helper Functions API Reference

This document provides a comprehensive reference for the eBPF `struct_ops` hooks, custom kernel helper functions, context structure, and policy lifecycle used in the Linux Tiered Memory Management Framework.

---

## 1. Overview of Architecture

The Tiered Memory Subsystem decouples page profiling and placement decisions from kernel execution using **BPF `struct_ops`** (`struct tiered_mem_ops`). 

- **Kernel Core**: Samples access events (via PEBS or software samplers) and passes page metadata to registered BPF policy programs.
- **eBPF Policy (`struct_ops`)**: Maintains page access counters/statistics using dedicated BPF helpers, performs periodic ageing/decay, and classifies pages for promotion or demotion.

---

## 2. eBPF Context Structure (`struct tiered_mem_ebpf_ctx`)

Defined in `<uapi/linux/bpf.h>` and passed directly into `track_access` and `classify_page` hooks.

```c
struct tiered_mem_ebpf_ctx {
    __u64 pfn;              /* Page Frame Number */
    __u32 nid;              /* NUMA node ID where the page currently resides */
    __u32 access_count;     /* Hardware PEBS / software sampler access count */
    __u32 is_lru;           /* 1 if page is currently on an LRU list */
    __u32 is_active;        /* 1 if page is on the active LRU list */
    __u32 page_order;       /* Page allocation order (0 for 4KB base pages) */
    __u32 is_referenced;    /* 1 if page has PG_referenced flag set */
    __u32 is_dirty;         /* 1 if page is marked dirty */
    __u32 is_writeback;     /* 1 if page is under writeback IO */
    __u32 pid;              /* PID (Task ID) of process accessing the page */
    __u32 tgid;             /* TGID (Process ID / Thread Group ID) */
    __u64 zone_free_pages;  /* Number of free pages in the page's memory zone */
    __u64 node_total_pages; /* Total managed pages on the page's NUMA node */
};
```

---

## 3. `struct_ops` Callbacks (`struct tiered_mem_ops`)

A BPF policy program implements `struct tiered_mem_ops` in section `SEC(".struct_ops")`.

```c
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
```

### Callback Descriptions

| Function Hook | Execution Context | Description |
| :--- | :--- | :--- |
| `int init(struct tiered_mem_ops *ops)` | Policy registration | Invoked once when `bpftool struct_ops register` loads the policy into the kernel. Used to allocate data structures (e.g., calling `bpf_tiered_mem_create_page_counters()`). Returns `0` on success. |
| `void track_access(struct tiered_mem_ebpf_ctx *ctx)` | High-frequency sampler (PEBS / Page fault) | Called whenever a hardware access sample or memory reference is detected for a PFN. Updates access counters using BPF helpers. |
| `void age_page(unsigned long pfn)` | Periodic kernel ageing worker | Invoked by the background ageing kthread for each monitored PFN to decay access counts according to eBPF policy logic. |
| `int classify_page(struct tiered_mem_ebpf_ctx *ctx)` | Page migration policy worker | Evaluates page context and access history to return a migration decision:<br>• `0`: No action / Neutral<br>• `1`: **Promote** page (e.g. move to DRAM node)<br>• `2`: **Demote** page (e.g. move to CXL node) |
| `int get_hot_pages(...)` / `get_cold_pages(...)` | Kernel policy scan (Optional) | Optional batch scanning callbacks for populating promotion/demotion target candidate lists. |

---

## 4. Kernel eBPF Helper Functions

The subsystem exposes 4 dedicated kernel helper functions (`enum bpf_func_id` 212–215) for managing per-PFN access counters atomically.

### 1. `bpf_tiered_mem_create_page_counters` (Helper ID: `212`)

Allocates an atomic counter array matching `max_pfn` in kernel virtual memory.

```c
void *bpf_tiered_mem_create_page_counters(void);
```

- **Returns**: Pointer (`void *`) to the allocated array, or `NULL` if allocation fails.
- **Usage**: Call inside `SEC("struct_ops/init")` to set up per-PFN state.

---

### 2. `bpf_tiered_mem_inc_page_counter` (Helper ID: `213`)

Atomically increments the access counter for a specified PFN.

```c
int bpf_tiered_mem_inc_page_counter(void *array_ptr, unsigned long pfn);
```

- **Arguments**:
  - `array_ptr`: Pointer to counter array created by helper `212`.
  - `pfn`: Page Frame Number to update.
- **Returns**: The updated access count (`int`) after increment.
- **Usage**: Call inside `SEC("struct_ops/track_access")`.

---

### 3. `bpf_tiered_mem_get_page_counter` (Helper ID: `214`)

Reads the current access count for a specified PFN.

```c
int bpf_tiered_mem_get_page_counter(void *array_ptr, unsigned long pfn);
```

- **Arguments**:
  - `array_ptr`: Pointer to counter array.
  - `pfn`: Target Page Frame Number.
- **Returns**: The current access count (`int`).
- **Usage**: Call inside `SEC("struct_ops/classify_page")`.

---

### 4. `bpf_tiered_mem_decay_page_counter` (Helper ID: `215`)

Applies a percentage decay factor to a PFN's access count.

```c
int bpf_tiered_mem_decay_page_counter(void *array_ptr, unsigned long pfn, unsigned int factor);
```

- **Arguments**:
  - `array_ptr`: Pointer to counter array.
  - `pfn`: Target Page Frame Number.
  - `factor`: Percentage factor (e.g. `50` for 50% decay: `new_count = (old_count * 50) / 100`).
- **Returns**: The new decayed access count (`int`).
- **Usage**: Call inside `SEC("struct_ops/age_page")`.

---

## 5. eBPF Implementation Example (`tiered_struct_ops.bpf.c`)

```c
#include <linux/bpf.h>

#ifndef SEC
#define SEC(NAME) __attribute__((section(NAME), used))
#endif

/* BPF Helper function prototypes */
static long (*bpf_trace_printk)(const char *fmt, unsigned int fmt_size, ...) = (void *) BPF_FUNC_trace_printk;
static void *(*bpf_tiered_mem_create_page_counters)(void) = (void *) 212;
static int (*bpf_tiered_mem_inc_page_counter)(void *array_ptr, unsigned long pfn) = (void *) 213;
static int (*bpf_tiered_mem_get_page_counter)(void *array_ptr, unsigned long pfn) = (void *) 214;
static int (*bpf_tiered_mem_decay_page_counter)(void *array_ptr, unsigned long pfn, unsigned int factor) = (void *) 215;

void *my_page_counters = 0;

SEC("struct_ops/init")
int tiered_init(struct tiered_mem_ops *ops)
{
    my_page_counters = bpf_tiered_mem_create_page_counters();
    return 0;
}

SEC("struct_ops/track_access")
void tiered_track_access(struct tiered_mem_ebpf_ctx *ctx)
{
    if (!ctx || !my_page_counters)
        return;
    bpf_tiered_mem_inc_page_counter(my_page_counters, ctx->pfn);
}

SEC("struct_ops/age_page")
void tiered_age_page(unsigned long pfn)
{
    if (!my_page_counters)
        return;
    /* Apply 50% decay factor */
    bpf_tiered_mem_decay_page_counter(my_page_counters, pfn, 50);
}

SEC("struct_ops/classify_page")
int tiered_classify_page(struct tiered_mem_ebpf_ctx *ctx)
{
    int count;

    if (!ctx || !my_page_counters)
        return 0;

    count = bpf_tiered_mem_get_page_counter(my_page_counters, ctx->pfn);
    if (count >= 10)
        return 1; /* Promote to Fast Tier (DRAM) */
    else if (count < 3)
        return 2; /* Demote to Slow Tier (CXL) */

    return 0; /* Keep in current tier */
}

SEC(".struct_ops")
struct tiered_mem_ops custom_pebs_ops = {
    .init = (void *)tiered_init,
    .track_access = (void *)tiered_track_access,
    .age_page = (void *)tiered_age_page,
    .classify_page = (void *)tiered_classify_page,
    .name = "custom_pebs",
};

char _license[] SEC("license") = "GPL";
```

---

## 6. Registration & Lifecycle

To register and activate a BPF policy:

```bash
# 1. Compile the eBPF program
clang -g -O2 -target bpf -c tiered_struct_ops.bpf.c -o tiered_struct_ops.bpf.o

# 2. Register via bpftool
sudo bpftool struct_ops register tiered_struct_ops.bpf.o

# 3. Unregister when done
sudo bpftool struct_ops unregister name custom_pebs
```
