# Modular Tiered Memory Framework with eBPF Policy Engine

This guide provides the complete step-by-step instructions to install dependencies, configure and build the kernel with eBPF support, create the root filesystem, boot the QEMU VM, compile and attach the eBPF policy, and verify migrations.

---

### Step 1: Install Host Build Dependencies
Before compiling the kernel, install the required build utilities. The `dwarves` package provides `pahole`, which is required to generate the kernel BTF (BPF Type Format) information.

```bash
sudo apt-get update
sudo apt-get install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev dwarves
```

### Step 2: Configure the Kernel
Run the configuration setup script to configure NUMA, debug options, and the required eBPF subsystem options (`CONFIG_BPF_SYSCALL`, `CONFIG_BPF_JIT`, `CONFIG_DEBUG_INFO_BTF`, etc.):

```bash
./qemu_setup/setup_configs.sh
```

### Step 3: Compile the Kernel
Build the kernel using all available CPU threads:

```bash
make -j$(nproc)
```

Once the compilation completes, the kernel boot image will be generated at `arch/x86/boot/bzImage`.

### Step 4: Create the Root Filesystem Image
Create the raw Ubuntu disk image. The script is pre-configured to install development tools (`gcc`, `make`, `clang`, `llvm`, `libbpf-dev`, `libelf-dev`) inside the rootfs so that you can compile the eBPF policies directly inside the VM.

```bash
sudo ./qemu_setup/create_rootfs.sh
```

### Step 5: Boot the QEMU VM
Launch QEMU with the compiled kernel and rootfs disk image. The VM is configured with 2 NUMA nodes: Node 0 (DRAM, 2GB, with all 4 vCPUs) and Node 1 (CXL memory-only, 2GB).

```bash
sudo ./qemu_setup/run_qemu.sh

# echo "PermitRootLogin yes" >> /etc/ssh/sshd_config
# systemctl restart ssh
# systemctl restart ssh


<!-- old: -->
<!-- cd /sys/kernel/tiered_memory
echo 0 > dram_nodes
echo 1 > cxl_nodes
echo 5 > hot_threshold
echo 0 > cold_threshold
echo 1000 > sampling_interval
echo 2000 > ageing_interval
echo 50 > ageing_factor
echo 2000 > ktierd_interval
echo 256 > promotion_batch
echo 256 > demotion_batch
echo 1 > enable -->


cd /sys/kernel/tiered_memory
echo 0 > dram_nodes
echo 1 > cxl_nodes

# Adjust scanner settings for testing
echo 262144 > max_scan
echo 1000 > sampling_interval
echo 20000 > ageing_interval
echo 50 > ageing_factor
echo 3 > hot_threshold
echo 0 > cold_threshold
echo 2000 > ktierd_interval
echo 256 > promotion_batch
echo 256 > demotion_batch
echo 1 > enable



stress-ng --vm 2 --vm-bytes 1G --timeout 300s

cat /sys/kernel/debug/tiered_memory/stats

cat /sys/kernel/debug/tiered_memory/page_stats



./qemu_setup/setup_configs.sh
grep CONFIG_LOCALVERSION .config
rm -f include/config/kernel.release
./scripts/config --set-str CONFIG_LOCALVERSION "-tiered-btf-4"
rm -f include/config/auto.conf
rm -f include/config/auto.conf.cmd
rm -f include/config/kernel.release
make olddefconfig
grep CONFIG_LOCALVERSION include/config/auto.conf

make -s kernelrelease

make -j$(nproc) 2>error.log
readelf -S vmlinux | grep BTF

sudo make modules_install
sudo make install


sudo update-grub

sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 7.2.0-rc4-tiered-btf"
sudo grub-editenv list

sudo reboot

echo 0 | sudo tee /sys/kernel/tiered_memory/target_pid


echo 4240 | sudo tee /sys/kernel/tiered_memory/target_pid



# hook4 enable/disable
# Enable tiered memory framework
echo 1 | sudo tee /sys/kernel/tiered_memory/enable

# Enable Hook 4 logging (enabled by default)
echo 1 | sudo tee /sys/kernel/tiered_memory/hook4_debug_enable

# Optional: filter by PID (0 = all PIDs)
echo 4370 | sudo tee /sys/kernel/tiered_memory/target_pid

# Watch output in dmesg
dmesg -w | grep "tiered_mem:HOOK4"
