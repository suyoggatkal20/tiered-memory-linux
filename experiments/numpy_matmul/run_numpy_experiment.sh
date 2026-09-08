#!/usr/bin/env bash
#
# run_numpy_experiment.sh
# Automated Test Runner for Python NumPy MatMul Real Application Sampler Verification
#

set -e

# Ensure script is executed with root privileges
if [ "$EUID" -ne 0 ]; then
  echo "[!] Error: This script must be run with sudo privilege."
  exit 1
fi

EXP_DIR="/home/ub-02/linux/experiments/numpy_matmul"
SYSFS_DIR="/sys/kernel/tiered_memory"
DEBUGFS_STATS="/sys/kernel/debug/tiered_memory/page_stats"
CSV_OUTPUT="$EXP_DIR/results/numpy_experiment_results.csv"
HOT_PFNS_FILE="/tmp/numpy_hot_pfns.txt"
COLD_PFNS_FILE="/tmp/numpy_cold_pfns.txt"
PAGE_STATS_SNAPSHOT="/tmp/page_stats_numpy_snapshot.txt"
WAIT_TIME_SECONDS=300   # 5 minutes testing duration

echo "======================================================="
echo "   Real Application Sampler Test: Python NumPy MatMul "
echo "======================================================="

# 1. Setup Tiered Memory configurations per run_bare_metal.md
echo "[1/6] Configuring Tiered Memory sysfs parameters..."
if [ ! -d "$SYSFS_DIR" ]; then
    echo "[!] Error: Sysfs path $SYSFS_DIR does not exist. Is the custom kernel booted?"
    exit 1
fi

echo 0 > "$SYSFS_DIR/dram_nodes"
echo 1 > "$SYSFS_DIR/cxl_nodes"
# echo 262144 > "$SYSFS_DIR/max_scan"
echo 1000 > "$SYSFS_DIR/sampling_interval"
echo 1000000 > "$SYSFS_DIR/samples_per_interval"
echo 20000 > "$SYSFS_DIR/ageing_interval"
echo 50 > "$SYSFS_DIR/ageing_factor"
echo 3 > "$SYSFS_DIR/hot_threshold"
echo 0 > "$SYSFS_DIR/cold_threshold"
echo 2000 > "$SYSFS_DIR/ktierd_interval"
echo 256 > "$SYSFS_DIR/promotion_batch"
echo 256 > "$SYSFS_DIR/demotion_batch"

# Turn off ageing and migration as requested for static PFN tracking
echo 0 > "$SYSFS_DIR/ageing_enabled"
echo 0 > "$SYSFS_DIR/ktierd_enabled"

# Enable framework
echo 1 > "$SYSFS_DIR/enable"
echo "[+] Sysfs parameters configured and framework enabled."

# 2. Check Python3 and NumPy dependency
echo "[2/6] Checking Python3 and NumPy installation..."
if ! python3 -c "import numpy" 2>/dev/null; then
    echo "[!] Installing numpy package..."
    pip3 install numpy || apt-get install -y python3-numpy
fi
echo "[+] Python3 and NumPy environment verified."

# 3. Clean up previous temp files and launch python workload
echo "[3/6] Starting NumPy workload in background..."
rm -f "$HOT_PFNS_FILE" "$COLD_PFNS_FILE" "$PAGE_STATS_SNAPSHOT"

python3 "$EXP_DIR/numpy_workload.py" > "$EXP_DIR/results/numpy_workload.log" 2>&1 &
WORKLOAD_PID=$!

# Ensure background process is cleaned up on script exit
cleanup() {
    echo ""
    echo "[*] Cleaning up background Python workload (PID: $WORKLOAD_PID)..."
    kill -9 "$WORKLOAD_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 4. Wait for PFN list files to be generated
echo "[4/6] Waiting for NumPy workload to resolve PFN lists..."
for i in {1..20}; do
    if [ -f "$HOT_PFNS_FILE" ] && [ -f "$COLD_PFNS_FILE" ]; then
        break
    fi
    sleep 1
done

if [ ! -s "$HOT_PFNS_FILE" ] || [ ! -s "$COLD_PFNS_FILE" ]; then
    echo "[!] Error: Failed to retrieve Hot/Cold PFN lists from Python workload."
    cat "$EXP_DIR/results/numpy_workload.log"
    exit 1
fi

HOT_COUNT=$(wc -l < "$HOT_PFNS_FILE")
COLD_COUNT=$(wc -l < "$COLD_PFNS_FILE")
echo "[+] Successfully captured $HOT_COUNT Hot PFNs and $COLD_COUNT Cold PFNs."

# 5. Wait for 5 minutes while workload runs
echo "[5/6] Running workload for $WAIT_TIME_SECONDS seconds (5 minutes)..."
ELAPSED=0
INTERVAL=30
while [ $ELAPSED -lt $WAIT_TIME_SECONDS ]; do
    sleep $INTERVAL
    ELAPSED=$((ELAPSED + INTERVAL))
    SAMPLES=$(cat /sys/kernel/debug/tiered_memory/stats | grep "Total PEBS Samples" | awk '{print $4}')
    echo "[*] Progress: ${ELAPSED}s / ${WAIT_TIME_SECONDS}s completed. Total PEBS Samples: ${SAMPLES}"
done

# 6. Extract page stats efficiently and output to CSV
echo "[6/6] Reading telemetry from debugfs and generating CSV report..."

# Dump page_stats ONCE to a snapshot file for fast batch searching
cat "$DEBUGFS_STATS" > "$PAGE_STATS_SNAPSHOT"

TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Write CSV Header
echo "Timestamp,PFN,Type,AccessCount,Node" > "$CSV_OUTPUT"

# Single-pass extraction using awk
awk -v ts="$TIMESTAMP" -v hot_file="$HOT_PFNS_FILE" -v cold_file="$COLD_PFNS_FILE" '
BEGIN {
    # Load Hot PFNs
    while ((getline line < hot_file) > 0) {
        gsub(/[ \t\r\n]/, "", line)
        if (line != "") hot_pfns[line] = 1
    }
    close(hot_file)

    # Load Cold PFNs
    while ((getline line < cold_file) > 0) {
        gsub(/[ \t\r\n]/, "", line)
        if (line != "") cold_pfns[line] = 1
    }
    close(cold_file)
}
{
    if ($0 ~ /^#/) next

    n = split($0, fields, ",")
    if (n >= 3) {
        pfn = fields[1]; gsub(/[ \t]/, "", pfn)
        node = fields[2]; gsub(/[ \t]/, "", node)
        cnt = fields[3]; gsub(/[ \t]/, "", cnt)

        if (pfn in hot_pfns) {
            hot_cnt[pfn] = cnt
            hot_node[pfn] = node
            found_hot[pfn] = 1
        }
        if (pfn in cold_pfns) {
            cold_cnt[pfn] = cnt
            cold_node[pfn] = node
            found_cold[pfn] = 1
        }
    }
}
END {
    # Process Hot PFNs
    for (pfn in hot_pfns) {
        cnt = (pfn in found_hot) ? hot_cnt[pfn] : 0
        node = (pfn in found_hot) ? hot_node[pfn] : -1
        print ts "," pfn ",Hot," cnt "," node >> "'"$CSV_OUTPUT"'"
        hot_sum += cnt
        hot_total++
    }

    # Process Cold PFNs
    for (pfn in cold_pfns) {
        cnt = (pfn in found_cold) ? cold_cnt[pfn] : 0
        node = (pfn in found_cold) ? cold_node[pfn] : -1
        print ts "," pfn ",Cold," cnt "," node >> "'"$CSV_OUTPUT"'"
        cold_sum += cnt
        cold_total++
    }

    printf "\n=======================================================\n"
    printf "          NUMPY MATMUL EXPERIMENT SUMMARY               \n"
    printf "=======================================================\n"
    printf "Hot PFNs Analyzed      : %d\n", hot_total
    printf "Hot Region Total Hits  : %d\n", hot_sum
    printf "Hot Avg Hits/Page      : %.2f\n\n", (hot_total > 0 ? hot_sum/hot_total : 0)

    printf "Cold PFNs Analyzed     : %d\n", cold_total
    printf "Cold Region Total Hits : %d\n", cold_sum
    printf "Cold Avg Hits/Page     : %.2f\n", (cold_total > 0 ? cold_sum/cold_total : 0)
    printf "=======================================================\n"

    if (hot_sum > 0 && cold_sum == 0) {
        printf "VERDICT: [ PASS ] - Sampler accurately identified hot NumPy matrix pages while cold pages remained at 0!\n"
    } else if (hot_sum > 0 && cold_sum < (hot_sum / 10)) {
        printf "VERDICT: [ PASS (MINOR NOISE) ] - Hot matrix region recorded significantly higher hits than cold region.\n"
    } else {
        printf "VERDICT: [ FAIL / LOW SAMPLES ] - Hot count too low or cold count contaminated.\n"
    }
    printf "=======================================================\n"
}
' "$PAGE_STATS_SNAPSHOT"

echo ""
echo "[+] Experiment complete. Results saved to:"
echo "    $CSV_OUTPUT"
