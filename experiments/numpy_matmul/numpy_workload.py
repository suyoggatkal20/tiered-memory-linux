#!/usr/bin/env python3
"""
numpy_workload.py - Real Application Memory Sampler Workload
Allocates a 2 GiB NumPy matrix (16,384 x 16,384 float64 elements).
Constantly performs matrix multiplication on a 32 MiB / 256 MiB sub-slice (Hot Region)
while leaving the remaining 1.75+ GiB completely idle (Cold Region).
"""

import os
import sys
import time
import numpy as np

PAGE_SIZE = 4096

def get_pfn_from_vaddr(vaddr):
    """Read /proc/self/pagemap to resolve virtual address to physical frame number (PFN)."""
    try:
        with open("/proc/self/pagemap", "rb") as f:
            offset = (vaddr // PAGE_SIZE) * 8
            f.seek(offset)
            buf = f.read(8)
            if len(buf) < 8:
                return 0
            entry = int.from_bytes(buf, byteorder='little')
            if entry & (1 << 63):  # Present bit
                return entry & ((1 << 55) - 1)
    except Exception as e:
        pass
    return 0

def main():
    print("[+] Initializing NumPy MatMul Real Application Workload...")
    
    # Create array where rows are C-contiguous in memory
    # Total shape: 24,576 rows x 2,048 cols float64 = 402,653,184 bytes (384 MiB)
    # Hot slice : Rows 0 to 8191 (8192 rows x 2048 cols) = 134 MiB C-contiguous (Exceeds 80 MB L3 cache)
    # Cold slice: Rows 8192 to 24575 (16384 rows x 2048 cols) = 250 MiB
    TOTAL_ROWS = 24576
    HOT_ROWS = 8192
    COLS = 2048
    
    print(f"[+] Allocating 384 MiB C-contiguous NumPy matrix ({TOTAL_ROWS}x{COLS} float64)...")
    A = np.ones((TOTAL_ROWS, COLS), dtype=np.float64)
    
    # Touch all pages to force physical RAM allocation
    A += 1.0
    
    vaddr_base = A.ctypes.data
    print(f"[+] Matrix virtual base address: 0x{vaddr_base:x}")
    
    print("[+] Resolving physical PFNs for Hot and Cold regions...")
    hot_pfns = []
    cold_pfns = []
    
    bytes_per_row = COLS * 8
    # Sample PFNs from Hot Region (Rows 0 to HOT_ROWS-1)
    for row in range(0, HOT_ROWS, 100):
        vaddr = vaddr_base + (row * bytes_per_row)
        pfn = get_pfn_from_vaddr(vaddr)
        if pfn > 0:
            hot_pfns.append(pfn)
            
    # Sample PFNs from Cold Region (Rows HOT_ROWS to TOTAL_ROWS-1)
    for row in range(HOT_ROWS, TOTAL_ROWS, 200):
        vaddr = vaddr_base + (row * bytes_per_row)
        pfn = get_pfn_from_vaddr(vaddr)
        if pfn > 0:
            cold_pfns.append(pfn)
            
    with open("/tmp/numpy_hot_pfns.txt", "w") as f:
        for pfn in hot_pfns:
            f.write(f"{pfn}\n")
            
    with open("/tmp/numpy_cold_pfns.txt", "w") as f:
        for pfn in cold_pfns:
            f.write(f"{pfn}\n")
            
    print(f"[+] Saved {len(hot_pfns)} Hot PFNs to /tmp/numpy_hot_pfns.txt")
    print(f"[+] Saved {len(cold_pfns)} Cold PFNs to /tmp/numpy_cold_pfns.txt")
    
    # Get C-contiguous slice (flags.c_contiguous == True)
    sub_A = A[0:HOT_ROWS, :]
    assert sub_A.flags.c_contiguous, "sub_A must be C-contiguous!"
    
    print("\n[+] Starting continuous MatMul on C-contiguous 134MB Hot Region (Rows 0-8191)...")
    print("[+] Cold Region (Rows 8192-24575, 250 MiB) remains completely idle.\n")
    
    cycle = 0
    while True:
        cycle += 1
        # Mutate first element so input array is unique per loop
        A[0, 0] += 0.00001
        
        # Execute matrix multiplication directly on physical pages of sub_A
        # Shape: (8192 x 2048) . (2048 x 8192) -> (8192 x 8192)
        B = np.dot(sub_A, sub_A.T)
        
        if cycle % 5 == 0:
            time.sleep(0.001)

if __name__ == "__main__":
    main()
