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

# eBPF Configuration
./scripts/config --enable CONFIG_BPF
./scripts/config --enable CONFIG_BPF_SYSCALL
./scripts/config --enable CONFIG_BPF_JIT
./scripts/config --enable CONFIG_BPF_EVENTS
./scripts/config --enable CONFIG_DEBUG_INFO_BTF

# Network Drivers for Real Machines (Intel Wifi, Common Ethernet, USB Ethernet)
./scripts/config --enable CONFIG_IWLWIFI
./scripts/config --enable CONFIG_IWLMVM
./scripts/config --enable CONFIG_IGB
./scripts/config --enable CONFIG_IXGBE
./scripts/config --enable CONFIG_I40E
./scripts/config --enable CONFIG_ICE
./scripts/config --enable CONFIG_MLX5_CORE
./scripts/config --enable CONFIG_MLX5_CORE_EN
./scripts/config --enable CONFIG_MLX5_CORE_IPOIB
./scripts/config --enable CONFIG_USB_NET_DRIVERS
./scripts/config --enable CONFIG_USB_USBNET
./scripts/config --enable CONFIG_USB_RTL8152
./scripts/config --enable CONFIG_USB_NET_AX8817X
./scripts/config --enable CONFIG_USB_NET_AX88179_178A

# Apply changes and generate updated config
echo "[+] Running 'make olddefconfig' to finalize config dependencies..."
make olddefconfig

echo "[+] Kernel configuration successfully updated!"
