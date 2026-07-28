#!/usr/bin/env bash
# Description: Configures the kernel .config for NUMA, Tiered Memory, and debugging options.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$KERNEL_DIR"

if [ ! -f .config ]; then
    echo "[-] .config file not found in $KERNEL_DIR. Creating default defconfig..."
    make defconfig
fi

echo "[+] Tuning kernel configuration for Tiered Memory and GDB Debugging..."

# Enable Tiered Memory Framework
./scripts/config --enable CONFIG_TIERED_MEMORY

# Ensure NUMA and Migration are enabled
./scripts/config --enable CONFIG_NUMA
./scripts/config --enable CONFIG_ACPI_NUMA
./scripts/config --enable CONFIG_NUMA_MIGRATION
./scripts/config --enable CONFIG_MIGRATION
./scripts/config --enable CONFIG_NUMA_MEMBLKS

# Debugging and symbol configurations (disable NONE, enable DWARF debug info)
./scripts/config --disable CONFIG_DEBUG_INFO_NONE
./scripts/config --enable CONFIG_DEBUG_INFO
./scripts/config --enable CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
./scripts/config --enable CONFIG_GDB_SCRIPTS
./scripts/config --enable CONFIG_FRAME_POINTER
./scripts/config --enable CONFIG_KALLSYMS
./scripts/config --enable CONFIG_KALLSYMS_ALL

# Tracing and event configuration
./scripts/config --enable CONFIG_FTRACE
./scripts/config --enable CONFIG_FUNCTION_TRACER
./scripts/config --enable CONFIG_FUNCTION_GRAPH_TRACER
./scripts/config --enable CONFIG_STACK_TRACER
./scripts/config --enable CONFIG_DYNAMIC_DEBUG

# Apply changes and generate updated config
echo "[+] Running 'make olddefconfig' to finalize config dependencies..."
make olddefconfig

echo "[+] Kernel configuration successfully updated!"
