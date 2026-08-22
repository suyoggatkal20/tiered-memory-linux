#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>

#define PAGE_SIZE  4096
#define NUM_PAGES  2048		/* 8 MB working set — large enough for PEBS to sample */

int main(void)
{
	pid_t pid = getpid();
	int i;
	volatile char *pages[NUM_PAGES];
	unsigned long long total_accesses = 0;
	struct timespec ts_start, ts_now;

	printf("====================================================\n");
	printf("[+] Test Page Access Workload Started!\n");
	printf("[+] Process PID: %d\n", pid);
	printf("[+] Working set: %d pages (%d MB)\n",
	       NUM_PAGES, (NUM_PAGES * PAGE_SIZE) / (1024 * 1024));
	printf("====================================================\n");

	/* Allocate and fault-in all pages */
	for (i = 0; i < NUM_PAGES; i++) {
		pages[i] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
		if (pages[i] == MAP_FAILED) {
			perror("mmap failed");
			return 1;
		}
		/* Write to each page to ensure it is faulted in and dirty */
		pages[i][0] = (char)(i & 0xFF);
	}

	printf("[+] All %d pages allocated and faulted in.\n", NUM_PAGES);
	printf("[+] Continuously accessing pages in a hot loop...\n");
	printf("    (Press Ctrl+C to stop)\n\n");

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	while (1) {
		/*
		 * Tight loop over all pages — each iteration touches every
		 * page at least once, generating thousands of memory load/
		 * store events per second that PEBS can sample.
		 */
		for (i = 0; i < NUM_PAGES; i++) {
			pages[i][0] = (char)(total_accesses & 0xFF);
			(void)pages[i][0];  /* read-back */
		}
		total_accesses += NUM_PAGES;

		if (total_accesses % (10000000ULL) < (unsigned long long)NUM_PAGES) {
			clock_gettime(CLOCK_MONOTONIC, &ts_now);
			double elapsed = (ts_now.tv_sec - ts_start.tv_sec) +
					 (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
			printf("[+] PID %d: %llu million accesses in %.1f sec "
			       "(%.1f M/sec)\n",
			       pid, total_accesses / 1000000ULL, elapsed,
			       (total_accesses / 1e6) / elapsed);
		}
	}

	/* unreachable, but clean up anyway */
	for (i = 0; i < NUM_PAGES; i++)
		munmap((void *)pages[i], PAGE_SIZE);
	return 0;
}
