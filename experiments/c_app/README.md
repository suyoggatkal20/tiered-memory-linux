# Low-Level C Application Memory Sampler Experiment

This directory contains a low-level C application memory sampler test (`c_real_app.c`) designed to evaluate hardware PEBS sampler accuracy without high-level runtime or library abstractions.

---

## Directory Structure

```text
/home/ub-02/linux/experiments/c_app/
├── c_real_app.c        # C workload allocating 800MB and running dependent pointer-chasing on 200MB
├── run_c_experiment.sh # Automated sudo test runner
├── README.md           # Documentation
└── results/
    ├── c_workload.log  # Runtime output logs
    └── c_experiment_results.csv # Recorded CSV results
```

---

## How the Experiment Works

1. **Memory Allocation**:
   * Allocates **800 MB total physical memory** (`TOTAL_PAGES = 200000`).
   * Defines a **200 MB Hot Region** (`ACTIVE_PAGES = 50000`) and a **600 MB Cold Region** (150,000 pages).

2. **Pointer-Chasing Architecture**:
   * Builds a single closed Hamiltonian pointer-chasing cycle across all 50,000 active pages (`curr = *p`).
   * Because pointer load addresses depend on previous load values, the CPU hardware prefetcher cannot speculate, forcing **100% L3 cache misses** across the 200 MB working set.
   * The 600 MB Cold Region is never dereferenced.

3. **Telemetry & Verification**:
   * Resolves physical frame numbers via `/proc/self/pagemap` and exports Hot PFNs to `/tmp/c_app_hot_pfns.txt` and Cold PFNs to `/tmp/c_app_cold_pfns.txt`.
   * Runs for 5 minutes and dumps `/sys/kernel/debug/tiered_memory/page_stats`.
   * Evaluates sample hit rates and outputs results to `results/c_experiment_results.csv`.

---

## How to Run

Execute the test runner as root:

```bash
sudo /home/ub-02/linux/experiments/c_app/run_c_experiment.sh
```
