/*
 * cxl_workload.c
 * Synthetic CXL Memory Promotion Benchmark
 *
 * Allocates 500 MB directly on CXL NUMA Node 1 using numa_alloc_onnode(),
 * touches all pages to guarantee initial residence on Node 1, and builds
 * a single closed Hamiltonian cycle covering ALL pages to prevent small cycles.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <numa.h>
#include <numaif.h>

#define PAGE_SIZE 4096
#define TOTAL_PAGES 125000   // 500 MB total allocation
#define CXL_NODE 1

static void shuffle(uint32_t *a, uint32_t n) {
    for (uint32_t i = n - 1; i > 0; i--) {
        uint32_t j = (uint32_t)rand() % (i + 1);
        uint32_t tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

int main() {
    if (numa_available() < 0) {
        fprintf(stderr, "[!] Error: NUMA API is not available on this system.\n");
        return 1;
    }

    size_t size = (size_t)TOTAL_PAGES * PAGE_SIZE;
    printf("[+] Requesting %lu MB allocation on CXL NUMA Node %d...\n", size >> 20, CXL_NODE);

    // Allocate memory specifically bound to NUMA Node 1 (CXL)
    char *buf = (char *)numa_alloc_onnode(size, CXL_NODE);
    if (!buf) {
        perror("numa_alloc_onnode");
        return 1;
    }

    // Touch every page to force physical allocation on Node 1
    printf("[+] Touching all %d pages to fault memory into Node %d...\n", TOTAL_PAGES, CXL_NODE);
    for (size_t i = 0; i < TOTAL_PAGES; i++) {
        buf[i * PAGE_SIZE] = 'X';
    }

    // Build a single closed Hamiltonian cycle through ALL TOTAL_PAGES
    printf("[+] Constructing Hamiltonian pointer-chase cycle across all %d pages...\n", TOTAL_PAGES);
    uint32_t *pages = (uint32_t *)malloc(TOTAL_PAGES * sizeof(uint32_t));
    if (!pages) {
        perror("malloc");
        return 1;
    }

    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        pages[i] = i;
    }

    // Shuffle to randomize page access order and evade hardware prefetchers
    srand(12345);
    shuffle(pages, TOTAL_PAGES);

    // Link page[0] -> page[1] -> ... -> page[TOTAL_PAGES-1] -> page[0]
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        uint32_t current_idx = pages[i];
        uint32_t next_idx = pages[(i + 1) % TOTAL_PAGES];

        uint32_t *p = (uint32_t *)(buf + (size_t)current_idx * PAGE_SIZE);
        *p = next_idx;
    }

    printf("[+] Hamiltonian cycle setup complete. PID: %d\n", getpid());
    printf("[+] Starting rapid pointer chasing (100%% core utilization)...\n");
    fflush(stdout);

    uint32_t curr = pages[0];
    volatile uint32_t sink = 0;

    // Rapid continuous pointer chasing across all 500 MB
    while (1) {
        for (uint64_t step = 0; step < (uint64_t)TOTAL_PAGES * 100; step++) {
            volatile uint32_t *p = (volatile uint32_t *)(buf + (size_t)curr * PAGE_SIZE);
            curr = *p;
        }
        sink ^= curr;
    }

    free(pages);
    numa_free(buf, size);
    return 0;
}
