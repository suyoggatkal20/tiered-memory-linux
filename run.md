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
```
*(To exit the QEMU console, press `Ctrl+A` then `X`)*.

---

### Step 6: Install `bpftool` (Optional, Recommended for Debugging)

To inspect BPF programs, maps, and generate helper files like `vmlinux.h` in the guest VM, compile `bpftool` on the host and copy it to the guest VM:

1. **Compile `bpftool` on the host**:
   ```bash
   cd tools/bpf/bpftool
   make -j$(nproc)
   ```
2. **Copy the compiled `bpftool` binary to the VM**:
   ```bash
   scp -P 2222 bpftool root@localhost:/usr/local/bin/
   ```

---

### Step 7: Compile and Attach the eBPF Policy (Inside the VM)

Once logged into the VM, compile and load your custom migration policy:

1. **Compile the eBPF Policy Bytecode**:
   ```bash
   clang -O2 -target bpf -I/usr/include/x86_64-linux-gnu -c /root/policy_ebpf.c -o /root/policy_ebpf.o
   ```

2. **Compile the Loader Utility**:
   ```bash
   gcc -O2 /root/loader.c -o /root/loader -lbpf
   ```

3. **Run the Loader to Load & Attach the Policy**:
   ```bash
   /root/loader /root/policy_ebpf.o
   ```
   *Note: Press `Ctrl+C` once it displays `Successfully attached eBPF policy...`. The kernel retains the reference to the program.*

4. **Verify the Attached Status**:
   ```bash
   cat /sys/kernel/tiered_memory/ebpf_prog_fd
   ```
   It should output `attached`.

---

### Step 8: Configure and Enable the Tiered Memory Framework
Configure the NUMA layout and fine-tune the page scanning and ageing intervals:

```bash
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

# Enable the tiered memory framework
echo 1 > enable
```

### Step 9: Run Stress Workloads and Monitor Migrations
Run memory-intensive workloads to trigger access tracking and page migrations:


```bash
stress-ng --vm 2 --vm-bytes 1G --timeout 300s
```

Check the migration statistics in debugfs:

```bash
cat /sys/kernel/debug/tiered_memory/stats
```

### Step 10: View `bpf_printk` Logs
To view output print statements from the eBPF policy (`bpf_printk`), read the kernel trace pipe:
```bash
cat /sys/kernel/debug/tracing/trace_pipe
```
