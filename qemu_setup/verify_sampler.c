#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

#define PAGE_SIZE 4096
#define TOTAL_PAGES 100000    // 400 MB total memory allocation
#define ACTIVE_PAGES 50000     // 200 MB active memory (Vastly exceeds 80MB L3 cache)

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

    // Touch all pages to fault them into physical memory
    for (size_t i = 0; i < TOTAL_PAGES; i++) {
        buf[i * PAGE_SIZE] = 'A';
    }

    // Initialize array of all page indices
    uint32_t *all_pages = malloc(TOTAL_PAGES * sizeof(uint32_t));
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        all_pages[i] = i;
    }

    // Shuffle all pages randomly
    srand(42);
    shuffle(all_pages, TOTAL_PAGES);

    // The first ACTIVE_PAGES in all_pages form our ACTIVE set.
    // Build a single closed pointer-chasing cycle through all ACTIVE_PAGES:
    // Page all_pages[0] -> all_pages[1] -> ... -> all_pages[ACTIVE_PAGES-1] -> all_pages[0]
    for (uint32_t i = 0; i < ACTIVE_PAGES; i++) {
        uint32_t current_idx = all_pages[i];
        uint32_t next_idx = all_pages[(i + 1) % ACTIVE_PAGES];

        // Store the next page index inside the current page
        uint32_t *page_ptr = (uint32_t *)(buf + (size_t)current_idx * PAGE_SIZE);
        *page_ptr = next_idx;
    }

    FILE *f_active = fopen("/tmp/verify_active_pfns.txt", "w");
    FILE *f_inactive = fopen("/tmp/verify_inactive_pfns.txt", "w");

    printf("\n[+] Total Allocation: %lu MB (%d pages)\n", size >> 20, TOTAL_PAGES);
    printf("[+] Active Set Size : %d MB (%d pages in single closed cycle)\n", (ACTIVE_PAGES * PAGE_SIZE) >> 20, ACTIVE_PAGES);

    printf("\n=== ACTIVE PAGES (PART OF 200MB POINTER CHASE CYCLE) ===\n");
    for (int i = 0; i < 20; i++) {
        uint32_t idx = all_pages[i];
        uint64_t pfn = virt_to_pfn(buf + (size_t)idx * PAGE_SIZE);
        printf("Active Page Index %5d: Vaddr=%p -> PFN=%lu\n", idx, buf + (size_t)idx * PAGE_SIZE, pfn);
        if (f_active && pfn) fprintf(f_active, "%lu\n", pfn);
    }

    printf("\n=== INACTIVE PAGES (NOT IN CYCLE, NEVER ACCESSED) ===\n");
    for (int i = ACTIVE_PAGES; i < ACTIVE_PAGES + 20; i++) {
        uint32_t idx = all_pages[i];
        uint64_t pfn = virt_to_pfn(buf + (size_t)idx * PAGE_SIZE);
        printf("Inactive Page Index %5d: Vaddr=%p -> PFN=%lu\n", idx, buf + (size_t)idx * PAGE_SIZE, pfn);
        if (f_inactive && pfn) fprintf(f_inactive, "%lu\n", pfn);
    }

    if (f_active) fclose(f_active);
    if (f_inactive) fclose(f_inactive);

    printf("\n[+] PFN lists saved to /tmp/verify_active_pfns.txt and /tmp/verify_inactive_pfns.txt\n");
    printf("[+] Running single-cycle pointer chasing across all %d active pages...\n", ACTIVE_PAGES);

    uint32_t curr = all_pages[0];
    volatile uint32_t sink = 0;

    while (1) {
        // Traverse the closed cycle 10 full times per loop iteration
        for (uint64_t step = 0; step < (uint64_t)ACTIVE_PAGES * 10; step++) {
            volatile uint32_t *p = (volatile uint32_t *)(buf + (size_t)curr * PAGE_SIZE);
            curr = *p;
        }
        sink ^= curr;
        // usleep();
    }

    free(all_pages);
    munmap(buf, size);
    return 0;
}
