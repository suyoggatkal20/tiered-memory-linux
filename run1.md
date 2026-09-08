./qemu_setup/setup_configs.sh

make -j$(nproc)

sudo ./qemu_setup/create_rootfs.sh

sudo ./qemu_setup/run_qemu.sh

copy loader.c and policy_ebpf.c to the VM: 
scp loader.c policy_ebpf.c root@tiered-mem-vm:/root/

compile and run:

clang -O2 -target bpf -I/usr/include/x86_64-linux-gnu -c /root/policy_ebpf.c -o /root/policy_ebpf.o

gcc -O2 /root/loader.c -o /root/loader -lbpf

/root/loader /root/policy_ebpf.o


cd /sys/kernel/tiered_memory
echo 0 > dram_nodes
echo 1 > cxl_nodes
echo 262144 >  
echo 1000 > sampling_interval
echo 20000 > ageing_interval
echo 50 > ageing_factor
echo 3 > hot_threshold
echo 0 > cold_threshold
echo 2000 > ktierd_interval
echo 256 > promotion_batch
echo 256 > demotion_batch
echo 1 > enable

echo 0 > ageing_enabled



stress-ng --vm 2 --vm-bytes 1G --timeout 300s

cat /sys/kernel/debug/tiered_memory/stats

gcc /root/numa_chase.c -o /root/numa_chase

./root/numa_chase 20

cat /sys/kernel/debug/tracing/trace_pipe


cat /sys/kernel/debug/tiered_memory/page_stats > /home/ub-02/tiered_mem/mydata5.txt


