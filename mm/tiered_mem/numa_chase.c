#define _GNU_SOURCE
#include <numa.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define ALLOC_SIZE (1ULL << 30)   // 1 GiB

typedef struct {
    uint32_t next;
} page_node_t;

static void shuffle(uint32_t *a, uint64_t n)
{
    for (uint64_t i = n - 1; i > 0; i--) {
        uint64_t j = (uint64_t)rand() % (i + 1);

        uint32_t tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec +
           (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    unsigned long page_size;
    uint64_t num_pages;

    int numa_node = 1;
    uint64_t cycles = 1000;

    if (argc > 1)
        cycles = strtoull(argv[1], NULL, 10);

    page_size = sysconf(_SC_PAGESIZE);

    /*
     * Number of pages in 1 GiB.
     * With 4 KiB pages:
     *
     * 1 GiB / 4 KiB = 262144 pages
     */
    num_pages = ALLOC_SIZE / page_size;

    printf("Allocation size : %llu MiB\n",
           (unsigned long long)(ALLOC_SIZE >> 20));

    printf("Page size       : %lu bytes\n", page_size);

    printf("Number of pages : %llu\n",
           (unsigned long long)num_pages);

    printf("NUMA node       : %d\n", numa_node);

    printf("Cycles          : %llu\n",
           (unsigned long long)cycles);

    if (numa_available() < 0) {
        fprintf(stderr, "NUMA is not available\n");
        return 1;
    }

    if (numa_max_node() < numa_node) {
        fprintf(stderr,
                "NUMA node %d does not exist (max node = %d)\n",
                numa_node, numa_max_node());
        return 1;
    }

    /*
     * Allocate the memory directly on NUMA node 1.
     */
    page_node_t *memory =
        (page_node_t *)numa_alloc_onnode(ALLOC_SIZE, numa_node);

    if (!memory) {
        perror("numa_alloc_onnode");
        return 1;
    }

    printf("Memory allocated at %p\n", (void *)memory);

    /*
     * Allocate an array containing page numbers.
     */
    uint32_t *order =
        malloc(num_pages * sizeof(uint32_t));

    if (!order) {
        perror("malloc");
        numa_free(memory, ALLOC_SIZE);
        return 1;
    }

    /*
     * order[i] represents the page number.
     */
    for (uint64_t i = 0; i < num_pages; i++)
        order[i] = i;

    /*
     * Create a random permutation.
     */
    srand(12345);
    shuffle(order, num_pages);

    /*
     * Create ONE BIG CYCLE:
     *
     * page order[0] -> order[1] -> order[2] -> ...
     *                         -> order[N-1]
     *                         -> order[0]
     *
     * Therefore every cycle visits every page exactly once.
     */
    for (uint64_t i = 0; i < num_pages; i++) {

        uint32_t current = order[i];

        uint32_t next =
            order[(i + 1) % num_pages];

        page_node_t *current_page =
            (page_node_t *)((char *)memory +
                            (uint64_t)current * page_size);

        current_page->next = next;
    }

    /*
     * Make sure all pages have actually been touched.
     *
     * The writes above already touch every page, but this
     * explicit pass makes the intention clear.
     */
    for (uint64_t i = 0; i < num_pages; i++) {
        page_node_t *p =
            (page_node_t *)((char *)memory +
                            i * page_size);

        /*
         * Volatile prevents this access from being removed.
         */
        volatile uint32_t x = p->next;
        (void)x;
    }

    printf("Pointer-chasing cycle constructed.\n");
    printf("Every cycle will visit %llu pages.\n",
           (unsigned long long)num_pages);

    /*
     * Start at the first page in our random permutation.
     */
    uint32_t current = order[0];

    /*
     * Prevent the compiler from optimizing away the
     * pointer-chasing loop.
     */
    volatile uint32_t sink = 0;

    double start = get_time_sec();

    for (uint64_t c = 0; c < cycles; c++) {

        /*
         * Exactly num_pages pointer dereferences per cycle.
         *
         * Because "current" depends on the previous load,
         * this forms a true dependent pointer-chasing chain.
         */
        for (uint64_t i = 0; i < num_pages; i++) {

            page_node_t *p =
                (page_node_t *)((char *)memory +
                                (uint64_t)current * page_size);

            current = p->next;
        }

        sink ^= current;
    }

    double end = get_time_sec();

    double elapsed = end - start;

    uint64_t total_accesses =
        cycles * num_pages;

    printf("\n");
    printf("Finished.\n");
    printf("Total page accesses : %llu\n",
           (unsigned long long)total_accesses);

    printf("Elapsed time        : %.6f sec\n",
           elapsed);

    printf("Average access      : %.2f ns\n",
           elapsed * 1e9 / total_accesses);

    printf("Final index         : %u\n", sink);

    free(order);
    numa_free(memory, ALLOC_SIZE);

    return 0;
}