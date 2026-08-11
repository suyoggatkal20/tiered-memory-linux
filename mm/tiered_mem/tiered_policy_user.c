/*
 * Userspace loader for the Tiered Memory eBPF policy program.
 *
 * Usage: ./tiered_policy_user <bpf_object_file>
 *
 * This program:
 *   1. Opens the compiled BPF object (.o file)
 *   2. Sets the program type to BPF_PROG_TYPE_TIERED_MEM (33)
 *   3. Loads it into the kernel
 *   4. Writes the program FD to /sys/kernel/tiered_memory/ebpf_prog_fd
 *   5. Keeps running (holds the FD open) until Ctrl+C
 *
 * Compile: gcc -O2 -o tiered_policy_user tiered_policy_user.c -lbpf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

int print_libbpf_log(enum libbpf_print_level level, const char *format, va_list args)
{
	return vfprintf(stderr, format, args);
}

int main(int argc, char **argv)
{
	struct bpf_object *obj;
	struct bpf_program *prog;
	int prog_fd;
	char fd_str[32];
	int sysfs_fd;
	int err;

	libbpf_set_print(print_libbpf_log);

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <bpf_object_file>\n", argv[0]);
		return 1;
	}

	obj = bpf_object__open(argv[1]);
	if (!obj) {
		fprintf(stderr, "Failed to open BPF object %s\n", argv[1]);
		return 1;
	}

	prog = bpf_object__find_program_by_title(obj, "tiered_mem");
	if (!prog)
		prog = bpf_object__find_program_by_title(obj, "socket");
	if (!prog)
		prog = bpf_program__next(NULL, obj);
	if (!prog) {
		fprintf(stderr, "No programs found in BPF object\n");
		bpf_object__close(obj);
		return 1;
	}

	bpf_program__set_type(prog, 33);

	
	err = bpf_object__load(obj);
	if (err) {
		fprintf(stderr, "Failed to load BPF object: %d\n", err);
		bpf_object__close(obj);
		return 1;
	}

	prog_fd = bpf_program__fd(prog);
	if (prog_fd < 0) {
		fprintf(stderr, "Failed to get BPF program FD\n");
		bpf_object__close(obj);
		return 1;
	}

	printf("Successfully loaded BPF program! FD: %d\n", prog_fd);

	sysfs_fd = open("/sys/kernel/tiered_memory/ebpf_prog_fd", O_WRONLY);
	if (sysfs_fd < 0) {
		perror("Failed to open sysfs ebpf_prog_fd file");
		bpf_object__close(obj);
		return 1;
	}

	snprintf(fd_str, sizeof(fd_str), "%d", prog_fd);
	if (write(sysfs_fd, fd_str, strlen(fd_str)) < 0) {
		perror("Failed to write BPF program FD to sysfs");
		close(sysfs_fd);
		bpf_object__close(obj);
		return 1;
	}

	close(sysfs_fd);
	printf("Successfully attached eBPF policy to /sys/kernel/tiered_memory/ebpf_prog_fd!\n");

	printf("Press Ctrl+C to exit and detach...\n");
	while (1) {
		sleep(1);
	}

	bpf_object__close(obj);
	return 0;
}
