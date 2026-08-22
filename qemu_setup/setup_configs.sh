#!/usr/bin/env bash
# Description: Configures the kernel .config for NUMA, Tiered Memory, eBPF/BTF/struct_ops, Internet/Networking, QEMU VirtIO drivers, Filesystems, and GDB Debugging.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$KERNEL_DIR"

if [ ! -f .config ]; then
    echo "[-] .config file not found in $KERNEL_DIR. Creating default defconfig..."
    make defconfig
fi

echo "[+] Tuning kernel configuration for Tiered Memory, Full OS Networking, eBPF BTF/struct_ops, and Debugging..."

# 1. Tiered Memory, NUMA Framework & Real Hardware ACPI/CXL
./scripts/config --enable CONFIG_TIERED_MEMORY
./scripts/config --enable CONFIG_NUMA
./scripts/config --enable CONFIG_ACPI_NUMA
./scripts/config --enable CONFIG_NUMA_MIGRATION
./scripts/config --enable CONFIG_MIGRATION
./scripts/config --enable CONFIG_NUMA_MEMBLKS
./scripts/config --enable CONFIG_MEMORY_HOTPLUG
./scripts/config --enable CONFIG_MEMORY_HOTREMOVE
./scripts/config --enable CONFIG_ACPI_HMAT
./scripts/config --enable CONFIG_CXL_BUS
./scripts/config --enable CONFIG_CXL_MEM
./scripts/config --enable CONFIG_CXL_ACPI
./scripts/config --enable CONFIG_PERF_EVENTS
./scripts/config --enable CONFIG_X86_LOCAL_APIC
./scripts/config --enable CONFIG_ACPI

# 2. Hardware PEBS & PMU Performance Counters (Intel & AMD)
./scripts/config --enable CONFIG_PERF_EVENTS
./scripts/config --enable CONFIG_HW_PERF_EVENTS
./scripts/config --enable CONFIG_CPU_SUP_INTEL
./scripts/config --enable CONFIG_CPU_SUP_AMD
./scripts/config --enable CONFIG_PERF_EVENTS_INTEL_UNCORE
./scripts/config --enable CONFIG_X86_MSR
./scripts/config --enable CONFIG_X86_CPUID

# 3. Comprehensive eBPF, BTF & Struct_Ops Support
./scripts/config --enable CONFIG_BPF
./scripts/config --enable CONFIG_BPF_SYSCALL
./scripts/config --enable CONFIG_BPF_JIT
./scripts/config --enable CONFIG_BPF_JIT_ALWAYS_ON
./scripts/config --enable CONFIG_BPF_EVENTS
./scripts/config --enable CONFIG_BPF_STREAM_PARSER
./scripts/config --enable CONFIG_DEBUG_INFO_BTF
./scripts/config --enable CONFIG_DEBUG_INFO_BTF_MODULES
./scripts/config --enable CONFIG_BPF_LSM
./scripts/config --enable CONFIG_NETFILTER_BPF_LINK
./scripts/config --enable CONFIG_CGROUP_BPF
./scripts/config --enable CONFIG_KPROBES
./scripts/config --enable CONFIG_UPROBES
./scripts/config --enable CONFIG_KPROBE_EVENTS
./scripts/config --enable CONFIG_UPROBE_EVENTS

# 4. Full Internet Access, SSH & Physical/Virtual Networking (TCP/IP, Ethernet, Wi-Fi, IPv6)
./scripts/config --enable CONFIG_NET
./scripts/config --enable CONFIG_INET
./scripts/config --enable CONFIG_IPV6
./scripts/config --enable CONFIG_IP_PNP
./scripts/config --enable CONFIG_IP_PNP_DHCP
./scripts/config --enable CONFIG_IP_PNP_BOOTP
./scripts/config --enable CONFIG_NETDEVICES
./scripts/config --enable CONFIG_NET_CORE
./scripts/config --enable CONFIG_ETHERNET
./scripts/config --enable CONFIG_E1000
./scripts/config --enable CONFIG_E1000E
./scripts/config --enable CONFIG_IGB
./scripts/config --enable CONFIG_IXGBE
./scripts/config --enable CONFIG_I40E
./scripts/config --enable CONFIG_R8169
./scripts/config --enable CONFIG_BNX2X
./scripts/config --enable CONFIG_MLX4_EN
./scripts/config --enable CONFIG_MLX5_CORE
./scripts/config --enable CONFIG_MLX5_CORE_EN
./scripts/config --enable CONFIG_VIRTIO_NET
./scripts/config --enable CONFIG_TUN
./scripts/config --enable CONFIG_TAP
./scripts/config --enable CONFIG_PACKET
./scripts/config --enable CONFIG_UNIX
./scripts/config --enable CONFIG_UNIX98_PTYS
./scripts/config --enable CONFIG_LEGACY_PTYS
./scripts/config --enable CONFIG_NET_SCHED
./scripts/config --enable CONFIG_NETFILTER
./scripts/config --enable CONFIG_IP_NF_IPTABLES
./scripts/config --enable CONFIG_IP_NF_FILTER
./scripts/config --enable CONFIG_IP_NF_NAT
./scripts/config --enable CONFIG_IP_NF_TARGET_MASQUERADE
./scripts/config --enable CONFIG_CFG80211
./scripts/config --enable CONFIG_MAC80211

# 5. Real Machine NVMe/SATA Storage & QEMU VirtIO Drivers
./scripts/config --enable CONFIG_PCI
./scripts/config --enable CONFIG_PCI_MSI
./scripts/config --enable CONFIG_NVME_CORE
./scripts/config --enable CONFIG_BLK_DEV_NVME
./scripts/config --enable CONFIG_NVME_MULTIPATH
./scripts/config --enable CONFIG_BLK_DEV_SD
./scripts/config --enable CONFIG_SCSI
./scripts/config --enable CONFIG_ATA
./scripts/config --enable CONFIG_SATA_AHCI
./scripts/config --enable CONFIG_VIRTIO
./scripts/config --enable CONFIG_VIRTIO_PCI
./scripts/config --enable CONFIG_VIRTIO_BALLOON
./scripts/config --enable CONFIG_VIRTIO_CONSOLE
./scripts/config --enable CONFIG_VIRTIO_BLK
./scripts/config --enable CONFIG_VIRTIO_FS
./scripts/config --enable CONFIG_BLK_DEV_SD
./scripts/config --enable CONFIG_SCSI
./scripts/config --enable CONFIG_ATA
./scripts/config --enable CONFIG_SATA_AHCI
./scripts/config --enable CONFIG_SERIAL_8250
./scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
./scripts/config --enable CONFIG_TTY
./scripts/config --enable CONFIG_DEVTMPFS
./scripts/config --enable CONFIG_DEVTMPFS_MOUNT

# 6. EFI Boot & Real Machine Filesystems (Ext4, XFS, Btrfs, VFAT, OverlayFS)
./scripts/config --enable CONFIG_EFI
./scripts/config --enable CONFIG_EFI_STUB
./scripts/config --enable CONFIG_EFIVAR_FS
./scripts/config --enable CONFIG_EFI_PARTITION
./scripts/config --enable CONFIG_PARTITION_ADVANCED
./scripts/config --enable CONFIG_EXT4_FS
./scripts/config --enable CONFIG_EXT4_FS_POSIX_ACL
./scripts/config --enable CONFIG_EXT4_FS_SECURITY
./scripts/config --enable CONFIG_XFS_FS
./scripts/config --enable CONFIG_BTRFS_FS
./scripts/config --enable CONFIG_FAT_FS
./scripts/config --enable CONFIG_VFAT_FS
./scripts/config --enable CONFIG_OVERLAY_FS
./scripts/config --enable CONFIG_PROC_FS
./scripts/config --enable CONFIG_SYSFS
./scripts/config --enable CONFIG_TMPFS
./scripts/config --enable CONFIG_TMPFS_POSIX_ACL
./scripts/config --enable CONFIG_SQUASHFS
./scripts/config --enable CONFIG_NET_9P
./scripts/config --enable CONFIG_NET_9P_VIRTIO
./scripts/config --enable CONFIG_9P_FS
./scripts/config --enable CONFIG_9P_FS_POSIX_ACL

# 7. Process Control, Cgroups & Containers / Namespaces
./scripts/config --enable CONFIG_CGROUPS
./scripts/config --enable CONFIG_CGROUP_FREEZER
./scripts/config --enable CONFIG_CGROUP_PIDS
./scripts/config --enable CONFIG_CGROUP_DEVICE
./scripts/config --enable CONFIG_CGROUP_CPUACCT
./scripts/config --enable CONFIG_CGROUP_PERF
./scripts/config --enable CONFIG_MEMCG
./scripts/config --enable CONFIG_NAMESPACES
./scripts/config --enable CONFIG_UTS_NS
./scripts/config --enable CONFIG_IPC_NS
./scripts/config --enable CONFIG_USER_NS
./scripts/config --enable CONFIG_PID_NS
./scripts/config --enable CONFIG_NET_NS
./scripts/config --enable CONFIG_SECCOMP

# 8. Debugging, Symbol Tables, Ftrace & GDB Support
./scripts/config --disable CONFIG_DEBUG_INFO_NONE
./scripts/config --enable CONFIG_DEBUG_INFO
./scripts/config --enable CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
./scripts/config --enable CONFIG_GDB_SCRIPTS
./scripts/config --enable CONFIG_FRAME_POINTER
./scripts/config --enable CONFIG_KALLSYMS
./scripts/config --enable CONFIG_KALLSYMS_ALL
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
./scripts/config --enable CONFIG_KPROBES
./scripts/config --enable CONFIG_UPROBES

# 8. Apply changes and generate updated config
echo "[+] Running 'make olddefconfig' to finalize config dependencies..."
make olddefconfig

echo "[+] Kernel configuration successfully updated for Real Hardware & VM Deployment (Internet, SSH, NVMe, PEBS, eBPF, and Tiered Memory)!"
