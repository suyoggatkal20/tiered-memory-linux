
./qemu_setup/setup_configs.sh
make -j$(nproc)

sudo ./qemu_setup/create_rootfs.sh

numactl --hardware


sudo ./qemu_setup/run_qemu.sh

# echo "PermitRootLogin yes" >> /etc/ssh/sshd_config
# systemctl restart ssh
# systemctl restart ssh


cd /sys/kernel/tiered_memory
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
echo 1 > enable

stress-ng --vm 2 --vm-bytes 1G --timeout 300s

cat /sys/kernel/debug/tiered_memory/stats

cat /sys/kernel/debug/tiered_memory/page_stats

