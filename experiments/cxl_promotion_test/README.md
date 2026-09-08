# Synthetic CXL-to-DRAM Memory Promotion Experiment

This directory contains an automated end-to-end experiment setup designed to verify that active memory allocated on **CXL Node 1** is automatically detected and promoted to **DRAM Node 0** by the Tiered Memory framework.

---

## Directory Structure

```text
experiments/cxl_promotion_test/
├── README.md                 # Documentation and workflow guide
├── cxl_workload.c            # Synthetic workload (numa_alloc_onnode + Hamiltonian pointer chasing)
├── run_cxl_experiment.sh     # Automated 5-minute experiment runner
└── results/                  # Captured experiment runs
    └── <PREFIX>_<TIMESTAMP>/ # Dedicated folder for each experiment run
        ├── cxl_experiment_summary.csv
        ├── cxl_workload.log
        ├── numa_before.txt
        ├── numa_after_30s.txt
        └── numa_after.txt
```

---

## Workload Design (`cxl_workload.c`)

1. **Explicit CXL Allocation**:
   Uses `numa_alloc_onnode(500MB, 1)` to force physical allocation on **NUMA Node 1** (CXL node).
2. **Page Fault Initialization**:
   Touches all 125,000 pages to ensure 100% of memory starts on Node 1 before sampling begins.
3. **Hamiltonian Pointer Chasing**:
   Constructs a single closed random cycle traversing all 125,000 pages:
   $\text{Page}_0 \rightarrow \text{Page}_1 \rightarrow \dots \rightarrow \text{Page}_{124999} \rightarrow \text{Page}_0$.
   - **Zero Small Cycles**: Guarantees every single page in the 500 MB array is accessed before any page is repeated.
   - **Prefetcher Evasion**: Random permutation prevents CPU hardware prefetchers from caching accesses, forcing DRAM/CXL latency events.

---

## Execution Instructions

Run the test script with an optional custom prefix argument (e.g. `exp1`, `baseline`, `ebpf_policy`):

```bash
# Run with a custom prefix:
sudo /home/ub-02/linux/experiments/cxl_promotion_test/run_cxl_experiment.sh exp1

# Or run with default prefix ("run"):
sudo /home/ub-02/linux/experiments/cxl_promotion_test/run_cxl_experiment.sh
```

---

## Expected Outcome

1. **Before Framework Activation**: `numastat -p <PID>` shows 100% of memory on **Node 1 (CXL)** (~500 MB).
2. **During Benchmark (5 mins)**: `ktierd` detects high access counts on Node 1 PFNs and initiates page promotion to **Node 0 (DRAM)**.
3. **After Framework Completion**: `numastat -p <PID>` shows memory migrated from **Node 1** to **Node 0**, and `Total Promotions` reflects the migrated page count.
