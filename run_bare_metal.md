# Tiered Memory Kernel: Bare-Metal Setup & Configuration Guide

This guide describes how to compile, install, and configure the custom eBPF-enabled tiered memory kernel and framework directly on the physical bare-metal host.

---

## 1. Safety Step: Protect Network Connection (Before Rebooting)
To ensure you do not lose SSH access to the machine after booting into the new kernel, configure your netplan interfaces to match by physical MAC address. This prevents issues if the kernel changes the interface name from `eno1` to `enp1s0f0`.

1. Open your netplan configuration file:
   ```bash
   sudo nano /etc/netplan/01-netcfg.yaml
   ```
2. Update the configuration to match the Ethernet interface by its physical MAC address:
   ```yaml
   network:
     version: 2
     renderer: networkd
     ethernets:
       eno1:
         match:
           macaddress: "a4:bf:01:37:88:70"
         set-name: eno1
         addresses: [ 10.129.2.183/16 ]
         gateway4: 10.129.1.250
         nameservers:
             addresses:
                 - "10.200.1.11"
   ```
3. Apply the network configuration:
   ```bash
   sudo netplan apply
   ```

---

## 2. Setting Up Build Dependencies
Ensure all compile and package requirements are installed:
```bash
sudo apt update
sudo apt install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev \
                    pkg-config libbpf-dev clang llvm rsync kmod dwarves
```

---

## 3. Compiling the Kernel
With the driver configurations already verified in `.config` (built-in support for Intel `igb`, Mellanox `mlx5_core`, SATA `ahci`, and Ext4 file systems), compile the kernel:
```bash
# Clean any old build artifacts (optional, but recommended if changing configs)
make clean

# Compile the kernel using all available CPU threads
make -j$(nproc)
```

---

## 4. Installing the Kernel
Install the compiled modules and kernel image, then update your GRUB configuration:
```bash
# 1. Install kernel modules
sudo make modules_install

# 2. Install the kernel itself (copies files to /boot and generates initramfs)
sudo make install

# 3. Update bootloader menu entries
sudo update-grub

# 4. Get the kernel release name to use in grub-reboot
make -s kernelrelease   

# 5. Reboot in the new kernel
sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux <KERNEL_RELEASE_NAME>"

# 6. List the default kernel
sudo grub-editenv list

```

---

## 5. Rebooting into the New Kernel
Reboot the server. The bootloader will defaults to the newly installed kernel.
```bash
sudo reboot
```

### Post-Reboot Verification:
Once the system is back online, connect via SSH and run:
1. **Verify the kernel version**:
   ```bash
   uname -r
   # Should reflect the new custom version you built.
   ```
2. **Verify the network configuration**:
   ```bash
   ip a
   # Ensure eno1 is UP and has the IP 10.129.2.183.
   ```
3. **Verify the tiered memory sysfs is present**:
   ```bash
   ls -la /sys/kernel/tiered_memory/
   ```

---

## 6. Configuring and Enabling Tiered Memory
To configure Node 0 as DRAM and Node 1 as CXL, and set the sampling/migration parameters, configure the sysfs attributes:

```bash
# Go to tiered memory sysfs directory
cd /sys/kernel/tiered_memory

# 1. Set NUMA nodes
echo 0 > dram_nodes
echo 1 > cxl_nodes

# 2. Configure software scanner bounds
echo 262144 > max_scan
echo 1000 > sampling_interval      # Sample every 1000ms

# 3. Configure migration triggers (Ping-pong thresholds)
echo 3 > hot_threshold             # Promote page if count >= 3
echo 0 > cold_threshold            # Demote page if count <= 0

# 4. Configure migration daemon
echo 2000 > ktierd_interval        # Run ktierd loop every 2000ms
echo 256 > promotion_batch
echo 256 > demotion_batch

# 5. Disable ageing decay (recommended for initial testing/verification)
echo 0 > ageing_enabled

# 6. Enable the framework and migration daemon
echo 1 > ktierd_enabled
echo 1 > enable
```

---

## 7. Compiling and Loading the eBPF Policy
To attach a custom migration policy, compile your BPF program and load it into the kernel:

1. **Compile the eBPF policy code into BPF bytecode**:
   ```bash
   clang -O2 -target bpf -I/usr/include/x86_64-linux-gnu -c mm/tiered_mem/policy_ebpf.c -o policy_ebpf.o
   ```
2. **Compile the libbpf-based loader**:
   ```bash
   gcc -O2 mm/tiered_mem/loader.c -o loader -lbpf
   ```
3. **Load and attach the BPF policy**:
   ```bash
   sudo ./loader policy_ebpf.o
   ```

---

## 8. Monitoring Telemetry and Stats
Read telemetry values exposed by `/sys/kernel/debugfs/`:

*   **View framework metrics**:
    ```bash
    sudo cat /sys/kernel/debug/tiered_memory/stats
    ```
*   **View active page access counts in real-time**:
    ```bash
    sudo cat /sys/kernel/debug/tiered_memory/page_stats

    sudo cat /sys/kernel/debug/tiered_memory/page_stats | grep <ACTIVE_PFN>

    ```
