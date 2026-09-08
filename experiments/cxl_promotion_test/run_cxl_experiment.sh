#!/usr/bin/env bash
#
# run_cxl_experiment.sh
# Automated Test Runner for CXL to DRAM Memory Promotion Verification
#

set -e

# Ensure script is executed with root privileges
if [ "$EUID" -ne 0 ]; then
  echo "[!] Error: This script must be run with sudo privilege."
  exit 1
fi

PREFIX="${1:-run}"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RUN_ID="${PREFIX}_${TIMESTAMP}"

EXP_DIR="/home/ub-02/linux/experiments/cxl_promotion_test"
SYSFS_DIR="/sys/kernel/tiered_memory"
DEBUGFS_STATS="/sys/kernel/debug/tiered_memory/stats"

RUN_DIR="$EXP_DIR/results/$RUN_ID"
mkdir -p "$RUN_DIR"

CSV_OUTPUT="$RUN_DIR/cxl_experiment_summary.csv"
BEFORE_NUMA="$RUN_DIR/numa_before.txt"
AFTER_NUMA="$RUN_DIR/numa_after.txt"
WORKLOAD_LOG="$RUN_DIR/cxl_workload.log"
WAIT_TIME_SECONDS=300   # 5 minutes testing duration

echo "======================================================="
echo "   CXL-to-DRAM Tiered Memory Promotion Verification    "
echo "   Run Identifier: $RUN_ID"
echo "   Output Directory: $RUN_DIR"
echo "======================================================="

# 1. Compile synthetic CXL workload
echo "[1/6] Compiling cxl_workload.c with libnuma..."
gcc -O2 "$EXP_DIR/cxl_workload.c" -o "$EXP_DIR/cxl_workload" -lnuma
echo "[+] Workload binary compiled successfully."

# 2. Launch synthetic CXL workload in background
echo "[2/6] Launching 500 MB CXL workload in background..."
"$EXP_DIR/cxl_workload" > "$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!

cleanup() {
    echo ""
    echo "[*] Cleaning up background CXL workload (PID: $WORKLOAD_PID)..."
    kill -9 "$WORKLOAD_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for workload to finish initializing memory
sleep 4

if ! kill -0 "$WORKLOAD_PID" 2>/dev/null; then
    echo "[!] Error: CXL workload process failed to start."
    cat "$WORKLOAD_LOG"
    exit 1
fi

echo "[+] CXL Workload is running actively (PID: $WORKLOAD_PID)."

# 3. Capture Initial NUMA Memory Distribution BEFORE activating framework
echo "[3/6] Recording initial NUMA memory allocation for PID $WORKLOAD_PID..."
numastat -p "$WORKLOAD_PID" > "$BEFORE_NUMA"
cat "$BEFORE_NUMA"

# Parse initial Node 0 and Node 1 pages from numastat Total row
NODE0_BEFORE=$(grep "^Total" "$BEFORE_NUMA" | awk '{print $2}' || echo "0")
NODE1_BEFORE=$(grep "^Total" "$BEFORE_NUMA" | awk '{print $3}' || echo "0")

echo "[+] Initial Memory Stats: Node 0 (DRAM) = ${NODE0_BEFORE} MB | Node 1 (CXL) = ${NODE1_BEFORE} MB"

# 4. Configure Tiered Memory sysfs parameters
echo "[4/6] Configuring Tiered Memory sysfs parameters..."
if [ ! -d "$SYSFS_DIR" ]; then
    echo "[!] Error: Sysfs path $SYSFS_DIR does not exist. Is the custom kernel booted?"
    exit 1
fi

echo 0 > "$SYSFS_DIR/dram_nodes"
echo 1 > "$SYSFS_DIR/cxl_nodes"
echo 1000 > "$SYSFS_DIR/sampling_interval"
echo 1000000 > "$SYSFS_DIR/samples_per_interval"
echo 20000 > "$SYSFS_DIR/ageing_interval"
echo 50 > "$SYSFS_DIR/ageing_factor"
echo 2 > "$SYSFS_DIR/hot_threshold"
echo 0 > "$SYSFS_DIR/cold_threshold"
echo 1000 > "$SYSFS_DIR/ktierd_interval"
echo 1024 > "$SYSFS_DIR/promotion_batch"
echo 1024 > "$SYSFS_DIR/demotion_batch"

echo 0 > "$SYSFS_DIR/ageing_enabled"
echo 1 > "$SYSFS_DIR/ktierd_enabled"

# Reset and re-enable framework to start clean sampling & migration
echo 0 > "$SYSFS_DIR/enable"
echo 1 > "$SYSFS_DIR/enable"
echo "[+] Sysfs parameters configured. Sampling and ktierd promotion engine ENABLED."

# 5. Run for 5 minutes and track live promotions
echo "[5/6] Running benchmark for $WAIT_TIME_SECONDS seconds (5 minutes)..."
ELAPSED=0
INTERVAL=30

while [ $ELAPSED -lt $WAIT_TIME_SECONDS ]; do
    sleep $INTERVAL
    ELAPSED=$((ELAPSED + INTERVAL))
    
    PROMOTIONS=$(cat "$DEBUGFS_STATS" | grep "Total Promotions:" | awk '{print $3}')
    SAMPLES=$(cat "$DEBUGFS_STATS" | grep "Total PEBS Samples:" | awk '{print $4}')
    FAILURES=$(cat "$DEBUGFS_STATS" | grep "Promotion Failures:" | awk '{print $3}')
    numastat -p "$WORKLOAD_PID" > "$RUN_DIR/numa_after_${ELAPSED}s.txt"
    cat "$RUN_DIR/numa_after_${ELAPSED}s.txt"
    echo "[*] Progress: ${ELAPSED}s / ${WAIT_TIME_SECONDS}s | PEBS Samples: ${SAMPLES} | Promotions: ${PROMOTIONS} | Failures: ${FAILURES}"
done

# 6. Capture Final NUMA Memory Distribution AFTER 5 minutes
echo "[6/6] Capturing final NUMA memory statistics..."
numastat -p "$WORKLOAD_PID" > "$AFTER_NUMA"
cat "$AFTER_NUMA"


NODE0_AFTER=$(grep "^Total" "$AFTER_NUMA" | awk '{print $2}' || echo "0")
NODE1_AFTER=$(grep "^Total" "$AFTER_NUMA" | awk '{print $3}' || echo "0")

TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Save summary to CSV
echo "Timestamp,PID,Node0_Before_MB,Node1_Before_MB,Node0_After_MB,Node1_After_MB,Total_Promotions,PEBS_Samples" > "$CSV_OUTPUT"
echo "$TIMESTAMP,$WORKLOAD_PID,$NODE0_BEFORE,$NODE1_BEFORE,$NODE0_AFTER,$NODE1_AFTER,$PROMOTIONS,$SAMPLES" >> "$CSV_OUTPUT"

# Display Summary Report
echo ""
echo "======================================================="
echo "       CXL TO DRAM PROMOTION EXPERIMENT SUMMARY        "
echo "======================================================="
echo "Workload Process PID   : $WORKLOAD_PID"
echo "Benchmark Duration     : 5 Minutes (300s)"
echo "-------------------------------------------------------"
echo "BEFORE Framework       : Node 0 (DRAM) = ${NODE0_BEFORE} MB | Node 1 (CXL) = ${NODE1_BEFORE} MB"
echo "AFTER Framework        : Node 0 (DRAM) = ${NODE0_AFTER} MB | Node 1 (CXL) = ${NODE1_AFTER} MB"
echo "-------------------------------------------------------"
echo "Total PEBS Samples     : $SAMPLES"
echo "Total Page Promotions  : $PROMOTIONS pages"
echo "======================================================="

# Calculate net migration count (convert floats/integers if needed)
PROMOTED_MB=$(echo "$NODE0_AFTER - $NODE0_BEFORE" | bc -l 2>/dev/null || echo "0")

if [ "$PROMOTIONS" -gt 0 ] || [ $(echo "$NODE0_AFTER > $NODE0_BEFORE" | bc -l 2>/dev/null || echo 0) -eq 1 ]; then
    echo "VERDICT: [ PASS ] - Memory successfully promoted from CXL Node 1 to DRAM Node 0!"
else
    echo "VERDICT: [ FAIL / NO MIGRATION ] - Memory remained on CXL Node 1. Check promotion parameters."
fi
echo "======================================================="
echo "[+] Results saved to $CSV_OUTPUT"
