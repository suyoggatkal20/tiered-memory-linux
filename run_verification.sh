#!/usr/bin/env bash
#
# verify_sampler_experiment.sh
# Automated Sampler Accuracy Verification Script
#

set -e

# Ensure script is executed with root privileges
if [ "$EUID" -ne 0 ]; then
  echo "[!] Error: This script must be run with sudo privilege."
  exit 1
fi

SYSFS_DIR="/sys/kernel/tiered_memory"
DEBUGFS_STATS="/sys/kernel/debug/tiered_memory/page_stats"
CSV_OUTPUT="/home/ub-02/linux/sampler_verification_results.csv"
ACTIVE_PFNS_FILE="/tmp/verify_active_pfns.txt"
INACTIVE_PFNS_FILE="/tmp/verify_inactive_pfns.txt"
PAGE_STATS_SNAPSHOT="/tmp/page_stats_snapshot.txt"
WAIT_TIME_SECONDS=300   # 5 minutes testing duration

echo "======================================================="
echo "   Tiered Memory Hardware Sampler Verification Test    "
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

# 2. Compile verify_sampler tool
echo "[2/6] Compiling verify_sampler..."
gcc -O2 /home/ub-02/linux/qemu_setup/verify_sampler.c -o /home/ub-02/linux/verify_sampler
echo "[+] verify_sampler compiled successfully."

# 3. Clean up previous temp files and launch verify_sampler
echo "[3/6] Starting verify_sampler workload in background..."
rm -f "$ACTIVE_PFNS_FILE" "$INACTIVE_PFNS_FILE" "$PAGE_STATS_SNAPSHOT"

/home/ub-02/linux/verify_sampler > /tmp/verify_sampler.log 2>&1 &
SAMPLER_PID=$!

# Ensure background process is cleaned up on script exit
cleanup() {
    echo ""
    echo "[*] Cleaning up background verify_sampler process (PID: $SAMPLER_PID)..."
    kill -9 "$SAMPLER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 4. Wait for PFN list files to be generated
echo "[4/6] Waiting for verify_sampler to initialize PFN lists..."
for i in {1..15}; do
    if [ -f "$ACTIVE_PFNS_FILE" ] && [ -f "$INACTIVE_PFNS_FILE" ]; then
        break
    fi
    sleep 1
done

if [ ! -s "$ACTIVE_PFNS_FILE" ] || [ ! -s "$INACTIVE_PFNS_FILE" ]; then
    echo "[!] Error: Failed to retrieve active/inactive PFN lists from verify_sampler."
    cat /tmp/verify_sampler.log
    exit 1
fi

ACTIVE_COUNT=$(wc -l < "$ACTIVE_PFNS_FILE")
INACTIVE_COUNT=$(wc -l < "$INACTIVE_PFNS_FILE")
echo "[+] Successfully captured $ACTIVE_COUNT active PFNs and $INACTIVE_COUNT inactive PFNs."

# 5. Wait for 5 minutes while sampling workload runs
echo "[5/6] Waiting $WAIT_TIME_SECONDS seconds (5 minutes) for sample accumulation..."
ELAPSED=0
INTERVAL=30
while [ $ELAPSED -lt $WAIT_TIME_SECONDS ]; do
    sleep $INTERVAL
    ELAPSED=$((ELAPSED + INTERVAL))
    REMAINING=$((WAIT_TIME_SECONDS - ELAPSED))
    SAMPLES=$(cat /sys/kernel/debug/tiered_memory/stats | grep "Total PEBS Samples" | awk '{print $4}')
    echo "[*] Progress: ${ELAPSED}s / ${WAIT_TIME_SECONDS}s completed. Total PEBS Samples recorded: ${SAMPLES}"
done

# 6. Extract page stats efficiently and output to CSV
echo "[6/6] Reading telemetry from debugfs and generating CSV report..."

# Dump page_stats ONCE to a snapshot file for fast batch searching
cat "$DEBUGFS_STATS" > "$PAGE_STATS_SNAPSHOT"

TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Write CSV Header
echo "Timestamp,PFN,Type,AccessCount,Node" > "$CSV_OUTPUT"

# Efficient single-pass extraction using awk
awk -v ts="$TIMESTAMP" -v act_file="$ACTIVE_PFNS_FILE" -v inact_file="$INACTIVE_PFNS_FILE" '
BEGIN {
    # Load Active PFNs
    while ((getline line < act_file) > 0) {
        gsub(/[ \t\r\n]/, "", line)
        if (line != "") active_pfns[line] = 1
    }
    close(act_file)

    # Load Inactive PFNs
    while ((getline line < inact_file) > 0) {
        gsub(/[ \t\r\n]/, "", line)
        if (line != "") inactive_pfns[line] = 1
    }
    close(inact_file)
}
{
    # Ignore comment header
    if ($0 ~ /^#/) next

    # Format: PFN, Node, AccessCount
    n = split($0, fields, ",")
    if (n >= 3) {
        pfn = fields[1]; gsub(/[ \t]/, "", pfn)
        node = fields[2]; gsub(/[ \t]/, "", node)
        cnt = fields[3]; gsub(/[ \t]/, "", cnt)

        if (pfn in active_pfns) {
            active_cnt[pfn] = cnt
            active_node[pfn] = node
            found_active[pfn] = 1
        }
        if (pfn in inactive_pfns) {
            inactive_cnt[pfn] = cnt
            inactive_node[pfn] = node
            found_inactive[pfn] = 1
        }
    }
}
END {
    # Process Active PFNs
    for (pfn in active_pfns) {
        cnt = (pfn in found_active) ? active_cnt[pfn] : 0
        node = (pfn in found_active) ? active_node[pfn] : -1
        print ts "," pfn ",Active," cnt "," node >> "'"$CSV_OUTPUT"'"
        active_sum += cnt
        active_total++
    }

    # Process Inactive PFNs
    for (pfn in inactive_pfns) {
        cnt = (pfn in found_inactive) ? inactive_cnt[pfn] : 0
        node = (pfn in found_inactive) ? inactive_node[pfn] : -1
        print ts "," pfn ",Inactive," cnt "," node >> "'"$CSV_OUTPUT"'"
        inactive_sum += cnt
        inactive_total++
    }

    printf "\n=======================================================\n"
    printf "                VERIFICATION SUMMARY                    \n"
    printf "=======================================================\n"
    printf "Active PFNs Analyzed   : %d\n", active_total
    printf "Active Total Samples   : %d\n", active_sum
    printf "Active Avg Samples/Page: %.2f\n\n", (active_total > 0 ? active_sum/active_total : 0)

    printf "Inactive PFNs Analyzed : %d\n", inactive_total
    printf "Inactive Total Samples : %d\n", inactive_sum
    printf "Inactive Avg Samples   : %.2f\n", (inactive_total > 0 ? inactive_sum/inactive_total : 0)
    printf "=======================================================\n"

    if (active_sum > 0 && inactive_sum == 0) {
        printf "VERDICT: [ PASS ] - Hardware sampler accurately recorded active pages while ignoring inactive pages!\n"
    } else if (active_sum > 0 && inactive_sum < (active_sum / 10)) {
        printf "VERDICT: [ PASS (MINOR NOISE) ] - Active pages recorded significantly higher hits than inactive pages.\n"
    } else {
        printf "VERDICT: [ FAIL / LOW SAMPLES ] - Active count too low or inactive count contaminated.\n"
    }
    printf "=======================================================\n"
}
' "$PAGE_STATS_SNAPSHOT"

echo ""
echo "[+] Verification complete. Detailed CSV report saved to:"
echo "    $CSV_OUTPUT"
