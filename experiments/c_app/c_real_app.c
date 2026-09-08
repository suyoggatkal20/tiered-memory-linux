/*
 * c_real_app.c - Low-Level C Real Application Memory Sampler Workload
 * Allocates 800 MB total physical memory (200,000 pages).
 * Constructs a 200 MB Active Hot Region (50,000 pages) using dependent pointer-chasing.
 * Leaves the 600 MB Cold Region (150,000 pages) completely untouched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

#define PAGE_SIZE 4096
#define TOTAL_PAGES 200000    // 800 MB total memory allocation
#define ACTIVE_PAGES 50000     // 200 MB active memory (Exceeds 80MB L3 cache)

uint64_t virt_to_pfn(void *vaddr) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;

    uintptr_t addr = (uintptr_t)vaddr;
    uint64_t vpfn = addr / PAGE_SIZE;
    uint64_t entry = 0;

    if (lseek(fd, vpfn * sizeof(uint64_t), SEEK_SET) == -1 ||
        read(fd, &entry, sizeof(uint64_t)) != sizeof(uint64_t)) {
        close(fd);
        return 0;
    }
    close(fd);
    return (entry & (1ULL << 63)) ? (entry & ((1ULL << 55) - 1)) : 0;
}

static void shuffle(uint32_t *a, uint32_t n) {
    for (uint32_t i = n - 1; i > 0; i--) {
        uint32_t j = (uint32_t)rand() % (i + 1);
        uint32_t tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

int main() {
    size_t size = (size_t)TOTAL_PAGES * PAGE_SIZE;
    char *buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // Touch all pages to force physical RAM allocation
    for (size_t i = 0; i < TOTAL_PAGES; i++) {
        buf[i * PAGE_SIZE] = 'A';
    }

    uint32_t *all_pages = malloc(TOTAL_PAGES * sizeof(uint32_t));
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        all_pages[i] = i;
    }

    // Shuffle pages to randomize physical layout
    srand(42);
    shuffle(all_pages, TOTAL_PAGES);

    // Build single closed Hamiltonian cycle for the 50,000 ACTIVE (Hot) pages
    for (uint32_t i = 0; i < ACTIVE_PAGES; i++) {
        uint32_t current_idx = all_pages[i];
        uint32_t next_idx = all_pages[(i + 1) % ACTIVE_PAGES];

        uint32_t *page_ptr = (uint32_t *)(buf + (size_t)current_idx * PAGE_SIZE);
        *page_ptr = next_idx;
    }

    FILE *f_hot = fopen("/tmp/c_app_hot_pfns.txt", "w");
    FILE *f_cold = fopen("/tmp/c_app_cold_pfns.txt", "w");

    printf("\n[+] Total Allocation: %lu MB (%d pages)\n", size >> 20, TOTAL_PAGES);
    printf("[+] Hot Region Size : %d MB (%d pages in single pointer-chase cycle)\n", (ACTIVE_PAGES * PAGE_SIZE) >> 20, ACTIVE_PAGES);
    printf("[+] Cold Region Size: %d MB (%d pages left untouched)\n", ((TOTAL_PAGES - ACTIVE_PAGES) * PAGE_SIZE) >> 20, TOTAL_PAGES - ACTIVE_PAGES);

    printf("\n=== EXPORTING HOT PFNs (%d pages) ===\n", ACTIVE_PAGES);
    for (int i = 0; i < ACTIVE_PAGES; i += 10) {
        uint32_t idx = all_pages[i];
        uint64_t pfn = virt_to_pfn(buf + (size_t)idx * PAGE_SIZE);
        if (f_hot && pfn) fprintf(f_hot, "%lu\n", pfn);
    }

    printf("=== EXPORTING COLD PFNs (%d pages) ===\n", TOTAL_PAGES - ACTIVE_PAGES);
    for (int i = ACTIVE_PAGES; i < TOTAL_PAGES; i += 30) {
        uint32_t idx = all_pages[i];
        uint64_t pfn = virt_to_pfn(buf + (size_t)idx * PAGE_SIZE);
        if (f_cold && pfn) fprintf(f_cold, "%lu\n", pfn);
    }

    if (f_hot) fclose(f_hot);
    if (f_cold) fclose(f_cold);

    printf("\n[+] PFN lists saved to /tmp/c_app_hot_pfns.txt and /tmp/c_app_cold_pfns.txt\n");
    printf("[+] Running continuous dependent pointer chasing across 200MB Hot Region...\n");

    uint32_t curr = all_pages[0];
    volatile uint32_t sink = 0;

    while (1) {
        // Continuous dependent pointer loads across active pages
        for (uint64_t step = 0; step < (uint64_t)ACTIVE_PAGES * 50; step++) {
            volatile uint32_t *p = (volatile uint32_t *)(buf + (size_t)curr * PAGE_SIZE);
            curr = *p;
        }
        sink ^= curr;
    }

    free(all_pages);
    munmap(buf, size);
    return 0;
}
