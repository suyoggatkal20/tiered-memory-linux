#!/usr/bin/env bash
# Description: Creates a minimal Ubuntu rootfs raw image using debootstrap for QEMU.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="$SCRIPT_DIR/ubuntu-rootfs.img"
IMAGE_SIZE="7G"
MOUNT_DIR="$SCRIPT_DIR/mnt_rootfs"
RELEASE="jammy" # Ubuntu 22.04 LTS

# Ensure script is run with sudo/root permissions since mount/debootstrap require them
if [ "$EUID" -ne 0 ]; then
    echo "[-] This script requires root permissions to mount loop devices and run debootstrap."
    echo "    Please run as: sudo ./create_rootfs.sh"
    exit 1
fi

echo "[+] Creating raw disk image of size $IMAGE_SIZE..."
qemu-img create -f raw "$IMAGE_NAME" "$IMAGE_SIZE"

echo "[+] Formatting disk image as ext4..."
mkfs.ext4 -F "$IMAGE_NAME"

echo "[+] Creating mount directory..."
mkdir -p "$MOUNT_DIR"

echo "[+] Mounting disk image..."
mount "$IMAGE_NAME" "$MOUNT_DIR"

# Cleanup trap to ensure we always unmount on failure or exit
cleanup() {
    echo "[+] Cleaning up mounts..."
    if mountpoint -q "$MOUNT_DIR"; then
        umount "$MOUNT_DIR"
    fi
    if [ -d "$MOUNT_DIR" ]; then
        rmdir "$MOUNT_DIR"
    fi
}
trap cleanup EXIT

echo "[+] Running debootstrap for Ubuntu $RELEASE (this may take a few minutes)..."
# Installs core utilities plus benchmark and diagnostics tools
debootstrap --arch=amd64 \
            --include=systemd,udev,dbus,iproute2,netplan.io,kmod,sudo,openssh-server,numactl,pciutils,procps,iputils-ping,nano,curl \
            "$RELEASE" "$MOUNT_DIR" http://archive.ubuntu.com/ubuntu/

echo "[+] Configuring system files..."

# 1. Set Hostname
echo "tiered-mem-vm" > "$MOUNT_DIR/etc/hostname"

# 2. Configure Localhost / etc/hosts
cat <<EOF > "$MOUNT_DIR/etc/hosts"
127.0.0.1   localhost
127.0.1.1   tiered-mem-vm

# The following lines are desirable for IPv6 capable hosts
::1         localhost ip6-localhost ip6-loopback
fe00::0     ip6-localnet
ff00::0     ip6-mcastprefix
ff02::1     ip6-allnodes
ff02::2     ip6-allrouters
EOF

# 3. Configure fstab
cat <<EOF > "$MOUNT_DIR/etc/fstab"
/dev/vda        /               ext4    defaults        0       1
devtmpfs        /dev            devtmpfs defaults       0       0
sysfs           /sys            sysfs   defaults        0       0
proc            /proc           proc    defaults        0       0
EOF

# 4. Set root password to 'root'
echo "root:root" | chroot "$MOUNT_DIR" chpasswd

# 5. Enable automatic passwordless login on the QEMU serial console (ttyS0)
mkdir -p "$MOUNT_DIR/etc/systemd/system/serial-getty@ttyS0.service.d"
cat <<EOF > "$MOUNT_DIR/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf"
[Service]
ExecStart=
ExecStart=-/sbin/agetty -o '-p -- \\\\u' --noclear --autologin root %I \$TERM
EOF

# 6. Configure network interface using netplan
mkdir -p "$MOUNT_DIR/etc/netplan"
cat <<EOF > "$MOUNT_DIR/etc/netplan/01-netcfg.yaml"
network:
  version: 2
  renderer: networkd
  ethernets:
    eth0:
      dhcp4: true
      match:
        name: e*
EOF

# 7. Configure apt sources to include universe repository
cat <<EOF > "$MOUNT_DIR/etc/apt/sources.list"
deb http://archive.ubuntu.com/ubuntu/ $RELEASE main restricted universe multiverse
deb http://archive.ubuntu.com/ubuntu/ $RELEASE-updates main restricted universe multiverse
deb http://security.ubuntu.com/ubuntu/ $RELEASE-security main restricted universe multiverse
EOF

echo "[+] Mounting dev, sys, proc inside rootfs to run additional configurations..."
mount --bind /dev "$MOUNT_DIR/dev"
mount --bind /sys "$MOUNT_DIR/sys"
mount --bind /proc "$MOUNT_DIR/proc"

# Clean up bind mounts on exit
cleanup_binds() {
    echo "[+] Unmounting dev, sys, proc bind mounts..."
    umount -l "$MOUNT_DIR/dev" || true
    umount -l "$MOUNT_DIR/sys" || true
    umount -l "$MOUNT_DIR/proc" || true
    cleanup
}
trap cleanup_binds EXIT

# 8. Enable sshd and networkd services, and install stress-ng from universe
chroot "$MOUNT_DIR" systemctl enable systemd-networkd
chroot "$MOUNT_DIR" systemctl enable ssh

echo "[+] Updating apt repositories and installing tools..."
chroot "$MOUNT_DIR" apt-get update
chroot "$MOUNT_DIR" apt-get install -y stress-ng gcc make clang llvm libbpf-dev libelf-dev

echo "[+] Rootfs image '$IMAGE_NAME' created and configured successfully!"
echo "[+] Default root password is: 'root'"
echo "[+] Autologin is configured on serial console ttyS0."


# echo "PermitRootLogin yes" >> /etc/ssh/sshd_config
# systemctl restart ssh
# systemctl restart ssh
