#!/usr/bin/env bash
# Description: Launches QEMU with the compiled kernel, Ubuntu rootfs, and 2 NUMA nodes (DRAM + CPU-less CXL).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

IMAGE_PATH="$SCRIPT_DIR/ubuntu-rootfs.img"
KERNEL_IMAGE="$KERNEL_DIR/arch/x86/boot/bzImage"

# Check if rootfs image exists
if [ ! -f "$IMAGE_PATH" ]; then
    echo "[-] Rootfs image not found at $IMAGE_PATH."
    echo "    Please build it first by running: sudo ./create_rootfs.sh"
    exit 1
fi

# Check if kernel bzImage exists
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "[-] Kernel image not found at $KERNEL_IMAGE."
    echo "    Please build the kernel first by running: make -j\$(nproc)"
    exit 1
fi

# Default settings
DEBUG_MODE=0
MEM_SIZE_NODE0="2G"
MEM_SIZE_NODE1="2G"
VCPUS=4

# Parse command line flags
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -d|--debug) DEBUG_MODE=1; shift ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
done

QEMU_CMD="qemu-system-x86_64"

# Check if KVM is available and user has permissions
KVM_FLAGS=""
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    echo "[+] KVM acceleration is available and writable. Enabling KVM..."
    KVM_FLAGS="-enable-kvm -cpu host"
else
    echo "[!] Warning: KVM acceleration not available or not writable by current user."
    echo "    Will fall back to TCG emulation (much slower) or run with 'sudo'."
    KVM_FLAGS="-cpu max"
fi

# Construct NUMA configuration
# Node 0: DRAM (2GB, contains all 4 vCPUs)
# Node 1: CXL  (2GB, CPU-less node)
NUMA_FLAGS=(
    "-m" "4G"
    "-smp" "$VCPUS"
    "-object" "memory-backend-ram,id=mem0,size=$MEM_SIZE_NODE0"
    "-numa" "node,nodeid=0,cpus=0-3,memdev=mem0"
    "-object" "memory-backend-ram,id=mem1,size=$MEM_SIZE_NODE1"
    "-numa" "node,nodeid=1,memdev=mem1"
)

# Network Configuration: Forward guest SSH port 22 to host port 2222
NET_FLAGS=(
    "-netdev" "user,id=net0,hostfwd=tcp::2222-:22"
    "-device" "virtio-net-pci,netdev=net0"
)

# Disk Configuration (virtio block device)
DISK_FLAGS=(
    "-drive" "file=$IMAGE_PATH,format=raw,if=virtio"
)

# Kernel boot command line arguments
# console=ttyS0 direct output to serial terminal
# earlyprintk=serial prints early boot logs to console
# nokaslr is required for reliable GDB breakpoint mapping
# tiered_memory=1 (or enable via sysfs)
BOOT_APPEND="root=/dev/vda console=ttyS0 earlyprintk=serial nokaslr tiered_memory=1"

# Debugging options
DEBUG_FLAGS=""
if [ "$DEBUG_MODE" -eq 1 ]; then
    echo "[+] Starting QEMU in DEBUG mode."
    echo "    GDB server will listen on port 1234."
    echo "    QEMU will wait for connection before booting. Run: gdb ./vmlinux, then: target remote :1234"
    DEBUG_FLAGS="-s -S"
fi

echo "[+] Booting VM with compiled kernel..."
echo "    Kernel: $KERNEL_IMAGE"
echo "    Rootfs: $IMAGE_PATH"
echo "    Memory Node 0 (DRAM): $MEM_SIZE_NODE0 (CPUs 0-3)"
echo "    Memory Node 1 (CXL): $MEM_SIZE_NODE1 (CPU-less)"

exec $QEMU_CMD \
    $KVM_FLAGS \
    -snapshot \
    "${NUMA_FLAGS[@]}" \
    "${NET_FLAGS[@]}" \
    "${DISK_FLAGS[@]}" \
    -kernel "$KERNEL_IMAGE" \
    -append "$BOOT_APPEND" \
    $DEBUG_FLAGS \
    -nographic
