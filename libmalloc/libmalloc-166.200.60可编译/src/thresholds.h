/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef __THRESHOLDS_H
#define __THRESHOLDS_H

/*
 * Tiny region size definitions; these are split into quanta of 16 bytes, 
 * 64520 blocks is the magical value of how many quanta we can fit in a 1mb
 * region including the region trailer and metadata.
 */
#define SHIFT_TINY_QUANTUM 4
#define SHIFT_TINY_CEIL_BLOCKS 16 // ceil(log2(NUM_TINY_BLOCKS))
#define TINY_QUANTUM (1 << SHIFT_TINY_QUANTUM)
#define NUM_TINY_BLOCKS 64520
#define NUM_TINY_CEIL_BLOCKS (1 << SHIFT_TINY_CEIL_BLOCKS)

/* 
 * Small region size definitions.
 *
 * We can only represent up to 1<<15 for msize; but we choose to stay
 * even below that to avoid the convention msize=0 => msize = (1<<15)
 */
#define SHIFT_SMALL_QUANTUM (SHIFT_TINY_QUANTUM + 5) // 9
#define SMALL_QUANTUM (1 << SHIFT_SMALL_QUANTUM)	 // 512 bytes
#define SHIFT_SMALL_CEIL_BLOCKS 14 // ceil(log2(NUM_SMALL_BLOCKs))
#define NUM_SMALL_BLOCKS 16319
#define NUM_SMALL_CEIL_BLOCKS (1 << SHIFT_SMALL_CEIL_BLOCKS)
#define SMALL_BLOCKS_ALIGN (SHIFT_SMALL_CEIL_BLOCKS + SHIFT_SMALL_QUANTUM) // 23

#if MALLOC_TARGET_64BIT
#define NUM_TINY_SLOTS 64 // number of slots for free-lists
#else // MALLOC_TARGET_64BIT
#define NUM_TINY_SLOTS 32 // number of slots for free-lists
#endif // MALLOC_TARGET_64BIT

/* 
 * The threshold above which we start allocating from the small
 * magazines. Computed from the largest allocation we can make
 * in the tiny region (currently 1008 bytes on 64-bit, and 
 * 496 bytes on 32-bit).
 */
#define SMALL_THRESHOLD ((NUM_TINY_SLOTS - 1) * TINY_QUANTUM)

/*
 * The threshold above which we start allocating from the large
 * "region" (ie. direct vm_allocates). The LARGEMEM size is used
 * on macOS and 64bit iOS with 16k pages, > 2gb and > 2 cores once
 * CONFIG_SMALL_CUTOFF_DYNAMIC is enabled (TODO: rdar://problem/35395572)
 * Must be a multiple of SMALL_QUANTUM (512 bytes)
 */
#if MALLOC_TARGET_IOS
#if MALLOC_TARGET_64BIT
#define LARGE_THRESHOLD (15 * 1024)						// <1 * 16k pages
#define LARGE_THRESHOLD_LARGEMEM (64 * 1024)			//  4 * 16k pages
#else
#define LARGE_THRESHOLD (15 * 1024)						// <4 * 4k pages
#define LARGE_THRESHOLD_LARGEMEM (64 * 1024)			// 16 * 4k pages
#endif
#else
#define LARGE_THRESHOLD (15 * 1024)						//  <4 * 4k pages
#define LARGE_THRESHOLD_LARGEMEM (127 * 1024)			// <32 * 4k pages
#endif

/*
 * The number of slots in the free-list for small blocks.  To avoid going to
 * vm system as often on large memory machines, increase the number of free list
 * spots above some amount of RAM installed in the system.
 */
#define NUM_SMALL_SLOTS (LARGE_THRESHOLD >> SHIFT_SMALL_QUANTUM)
#define NUM_SMALL_SLOTS_LARGEMEM (LARGE_THRESHOLD_LARGEMEM >> SHIFT_SMALL_QUANTUM)

/*
 * When all memory is touched after a copy, vm_copy() is always a lose
 * But if the memory is only read, vm_copy() wins over memmove() at 3 or 4 pages
 * (on a G3/300MHz)
 *
 * This must be >= LARGE_THRESHOLD
 */
#if MALLOC_TARGET_IOS && MALLOC_TARGET_64BIT
#define VM_COPY_THRESHOLD (48 * 1024)					// 3 * 16k pages
#define VM_COPY_THRESHOLD_LARGEMEM (96 * 1024)			// 6 * 16k pages
#else
#define VM_COPY_THRESHOLD (40 * 1024)					// 10 * 4k pages
#define VM_COPY_THRESHOLD_LARGEMEM (128 * 1024) 		// 32 * 4k pages
#endif

/*
 * Large entry cache (death row) sizes. The large cache is bounded with
 * an overall top limit size, each entry is allowed a given slice of
 * that limit.
 */
#if MALLOC_TARGET_64BIT
#define LARGE_ENTRY_CACHE_SIZE 16
#define LARGE_CACHE_SIZE_LIMIT ((vm_size_t)0x80000000) /* 2Gb */
#define LARGE_CACHE_SIZE_ENTRY_LIMIT (LARGE_CACHE_SIZE_LIMIT / LARGE_ENTRY_CACHE_SIZE)
#else // MALLOC_TARGET_64BIT
#define LARGE_ENTRY_CACHE_SIZE 8
#define LARGE_CACHE_SIZE_LIMIT ((vm_size_t)0x02000000) /* 32Mb */
#define LARGE_CACHE_SIZE_ENTRY_LIMIT (LARGE_CACHE_SIZE_LIMIT / LARGE_ENTRY_CACHE_SIZE)
#endif // MALLOC_TARGET_64BIT

/*
 * Large entry cache (death row) "flotsam" limits. Until the large cache
 * contains at least "high" bytes, the cache is not cleaned under memory
 * pressure. After that, memory pressure notifications cause cache cleaning
 * until the large cache drops below the "low" limit.
 */
#define SZONE_FLOTSAM_THRESHOLD_LOW (1024 * 512)
#define SZONE_FLOTSAM_THRESHOLD_HIGH (1024 * 1024)

/*
 * The magazine freelist array must be large enough to accomdate the allocation
 * granularity of both the tiny and small allocators. In addition, the last
 * slot in the list is special and reserved for coalesced regions bigger than
 * the overall max allocation size of the allocator.
 */
#define MAGAZINE_FREELIST_SLOTS (NUM_SMALL_SLOTS_LARGEMEM + 1)
#define MAGAZINE_FREELIST_BITMAP_WORDS ((MAGAZINE_FREELIST_SLOTS + 31) >> 5)

/*
 * Density threshold used in determining the level of emptiness before
 * moving regions to the recirc depot.
 */
#define DENSITY_THRESHOLD(a) \
	((a) - ((a) >> 2)) // "Emptiness" f = 0.25, so "Density" is (1 - f)*a. Generally: ((a) - ((a) >> -log2(f)))

/*
 * Minimum number of regions to retain in a recirc depot.
 */
#define DEFAULT_RECIRC_RETAINED_REGIONS 2

/* Sanity checks. */

MALLOC_STATIC_ASSERT(NUM_SMALL_SLOTS_LARGEMEM == LARGE_THRESHOLD_LARGEMEM >> SHIFT_SMALL_QUANTUM,
		"NUM_SMALL_SLOTS_LARGEMEM must match LARGE_THRESHOLD_LARGEMEM >> SHIFT_SMALL_QUANTUM");

MALLOC_STATIC_ASSERT(NUM_TINY_SLOTS <= NUM_SMALL_SLOTS_LARGEMEM,
		"NUM_TINY_SLOTS must be less than or equal to NUM_SMALL_SLOTS_LARGEMEM");

MALLOC_STATIC_ASSERT((LARGE_THRESHOLD % SMALL_QUANTUM) == 0,
		"LARGE_THRESHOLD must be a multiple of SMALL_QUANTUM");
MALLOC_STATIC_ASSERT((LARGE_THRESHOLD_LARGEMEM % SMALL_QUANTUM) == 0,
		"LARGE_THRESHOLD_LARGEMEM must be a multiple of SMALL_QUANTUM");

MALLOC_STATIC_ASSERT((LARGE_THRESHOLD / SMALL_QUANTUM) <= NUM_SMALL_SLOTS,
		"LARGE_THRESHOLD must be less than NUM_SMALL_SLOTS * SMALL_QUANTUM");
MALLOC_STATIC_ASSERT((LARGE_THRESHOLD_LARGEMEM / SMALL_QUANTUM) <=  NUM_SMALL_SLOTS_LARGEMEM,
		"LARGE_THRESHOLD_LARGEMEM must be less than NUM_SMALL_SLOTS * SMALL_QUANTUM");

MALLOC_STATIC_ASSERT(VM_COPY_THRESHOLD >= LARGE_THRESHOLD,
		"VM_COPY_THRESHOLD must be larger than LARGE_THRESHOLD");
MALLOC_STATIC_ASSERT(VM_COPY_THRESHOLD_LARGEMEM >= LARGE_THRESHOLD_LARGEMEM,
		"VM_COPY_THRESHOLD_LARGEMEM must be larger than LARGE_THRESHOLD_LARGEMEM");

#endif // __THRESHOLDS_H
