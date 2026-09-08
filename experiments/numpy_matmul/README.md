# NumPy Sliced MatMul Real-Application Experiment

This directory contains an automated, real-world application benchmark designed to test the eBPF Tiered Memory Sampler on a production data science / linear algebra workload using Python and NumPy.

---

## Directory Structure

```text
/home/ub-02/linux/experiments/numpy_matmul/
├── numpy_workload.py        # Python workload allocating 2 GiB array and performing sliced MatMul
├── run_numpy_experiment.sh  # Automated sudo test runner
├── README.md                # This documentation
└── results/
    ├── numpy_workload.log   # Runtime logs from the Python workload
    └── numpy_experiment_results.csv # CSV file containing PFN sample counts
```

---

## How the Experiment Works

1. **Memory Layout**:
   * The Python workload allocates a **2 GiB float64 NumPy matrix** (16,384 $\times$ 16,384 elements = 524,288 physical pages).
   * NumPy stores arrays in C-contiguous row-major order:
     * **Hot Region** (Rows 0 to 2047): Occupies the first **256 MiB** (65,536 pages).
     * **Cold Region** (Rows 2048 to 16383): Occupies the remaining **1.75 GiB** (458,752 pages).

2. **Workload Execution**:
   * The script mutates `A[0, 0]` on every iteration to guarantee uniqueness.
   * It executes continuous matrix multiplication (`np.dot`) **only on the 256 MiB Hot Region**.
   * The 1.75 GiB Cold Region remains completely idle.

3. **PFN Resolution & Export**:
   * Reads `/proc/self/pagemap` to extract physical PFNs for both the Hot and Cold memory regions.
   * Exports PFN lists to `/tmp/numpy_hot_pfns.txt` and `/tmp/numpy_cold_pfns.txt`.

4. **Telemetry Verification**:
   * Configures sysfs parameters per `run_bare_metal.md` (disables ageing and migration to freeze PFNs).
   * Runs for 5 minutes and snapshots `/sys/kernel/debug/tiered_memory/page_stats`.
   * Analyzes access counts for Hot PFNs vs Cold PFNs using single-pass `awk`.

---

## How to Run

Execute the test runner as root:

```bash
sudo /home/ub-02/linux/experiments/numpy_matmul/run_numpy_experiment.sh
```

---

## Expected Results

Upon completion, the script prints a summary table:
* **Hot Region Avg Hits/Page**: Should show high non-zero numbers (e.g. 50–500+ hits).
* **Cold Region Avg Hits/Page**: Should be **0** (or near 0).
* **Verdict**: Displays `[ PASS ]` if the sampler accurately identified hot pages while leaving cold pages at zero.
* Detailed CSV saved to: `results/numpy_experiment_results.csv`.
