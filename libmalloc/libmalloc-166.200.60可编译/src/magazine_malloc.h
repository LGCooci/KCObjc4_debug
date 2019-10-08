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

#ifndef __MAGAZINE_MALLOC_H
#define __MAGAZINE_MALLOC_H

// MARK: magazine_malloc

MALLOC_NOEXPORT
malloc_zone_t *
create_scalable_zone(size_t initial_size, unsigned debug_flags);

MALLOC_NOEXPORT
szone_t *
create_scalable_szone(size_t initial_size, unsigned debug_flags);

MALLOC_EXPORT
boolean_t
scalable_zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats, unsigned subzone);

MALLOC_NOEXPORT
extern int max_magazines;

MALLOC_NOEXPORT
extern int recirc_retained_regions;

// MARK: magazine_malloc utility functions

MALLOC_NOEXPORT
extern const
struct malloc_introspection_t szone_introspect;

MALLOC_NOEXPORT
void
szone_batch_free(szone_t *szone, void **to_be_freed, unsigned count);

MALLOC_NOEXPORT
unsigned
szone_batch_malloc(szone_t *szone, size_t size, void **results, unsigned count);

MALLOC_NOEXPORT
void *
szone_calloc(szone_t *szone, size_t num_items, size_t size);

MALLOC_NOEXPORT
void
szone_free(szone_t *szone, void *ptr);

MALLOC_NOEXPORT
void
szone_free_definite_size(szone_t *szone, void *ptr, size_t size);

MALLOC_NOEXPORT
size_t
szone_good_size(szone_t *szone, size_t size);

MALLOC_NOEXPORT
void *
szone_malloc(szone_t *szone, size_t size);

MALLOC_NOEXPORT
void *
szone_memalign(szone_t *szone, size_t alignment, size_t size);

MALLOC_NOEXPORT
size_t
szone_pressure_relief(szone_t *szone, size_t goal);

MALLOC_NOEXPORT
boolean_t
szone_claimed_address(szone_t *szone, void *ptr);

MALLOC_NOEXPORT
void *
szone_realloc(szone_t *szone, void *ptr, size_t new_size);

MALLOC_NOEXPORT
size_t
szone_size(szone_t *szone, const void *ptr);

MALLOC_NOEXPORT
size_t
szone_size_try_large(szone_t *szone, const void *ptr);

MALLOC_NOEXPORT
void *
szone_valloc(szone_t *szone, size_t size);

// MARK: tiny region allocator functions

MALLOC_NOEXPORT
boolean_t
tiny_check_region(rack_t *rack, region_t region, size_t region_index,
		unsigned counter);

MALLOC_NOEXPORT
void
tiny_finalize_region(rack_t *rack, magazine_t *tiny_mag_ptr);

MALLOC_NOEXPORT
int
tiny_free_detach_region(rack_t *rack, magazine_t *tiny_mag_ptr, region_t r);

MALLOC_NOEXPORT
boolean_t
tiny_free_list_check(rack_t *rack, grain_t slot, unsigned counter);

MALLOC_NOEXPORT
boolean_t
tiny_free_no_lock(rack_t *rack, magazine_t *tiny_mag_ptr, mag_index_t mag_index, region_t region, void *ptr, msize_t msize);

MALLOC_NOEXPORT
size_t
tiny_free_reattach_region(rack_t *rack, magazine_t *tiny_mag_ptr, region_t r);

MALLOC_NOEXPORT
void
tiny_free_scan_madvise_free(rack_t *rack, magazine_t *depot_ptr, region_t r);

MALLOC_NOEXPORT
kern_return_t
tiny_in_use_enumerator(task_t task, void *context, unsigned type_mask, szone_t *szone, memory_reader_t reader,
		vm_range_recorder_t recorder);

MALLOC_NOEXPORT
void *
tiny_malloc_from_free_list(rack_t *rack, magazine_t *tiny_mag_ptr, mag_index_t mag_index, msize_t msize);

MALLOC_NOEXPORT
void *
tiny_malloc_should_clear(rack_t *rack, msize_t msize, boolean_t cleared_requested);

MALLOC_NOEXPORT
void *
tiny_memalign(szone_t *szone, size_t alignment, size_t size, size_t span);

MALLOC_NOEXPORT
boolean_t
tiny_claimed_address(rack_t *rack, void *ptr);

MALLOC_NOEXPORT
void *
tiny_try_shrink_in_place(rack_t *rack, void *ptr, size_t old_size, size_t new_good_size);

MALLOC_NOEXPORT
boolean_t
tiny_try_realloc_in_place(rack_t *rack, void *ptr, size_t old_size, size_t new_size);

MALLOC_NOEXPORT
void
free_tiny(rack_t *rack, void *ptr, region_t tiny_region, size_t known_size);

MALLOC_NOEXPORT
size_t
tiny_size(rack_t *rack, const void *ptr);

MALLOC_NOEXPORT
unsigned
tiny_batch_malloc(szone_t *szone, size_t size, void **results, unsigned count);

MALLOC_NOEXPORT
void
tiny_batch_free(szone_t *szone, void **to_be_freed, unsigned count);

MALLOC_NOEXPORT
void
print_tiny_free_list(rack_t *rack);

MALLOC_NOEXPORT
void
print_tiny_region(boolean_t verbose, region_t region, size_t bytes_at_start, size_t bytes_at_end);

// MARK: small region allocation functions

MALLOC_NOEXPORT
boolean_t
small_check_region(rack_t *rack, region_t region, size_t region_index,
		unsigned counter);

MALLOC_NOEXPORT
void
small_finalize_region(rack_t *rack, magazine_t *small_mag_ptr);

MALLOC_NOEXPORT
int
small_free_detach_region(rack_t *rack, magazine_t *small_mag_ptr, region_t r);

MALLOC_NOEXPORT
boolean_t
small_free_list_check(rack_t *rack, grain_t slot, unsigned counter);

MALLOC_NOEXPORT
size_t
small_free_reattach_region(rack_t *rack, magazine_t *small_mag_ptr, region_t r);

MALLOC_NOEXPORT
void
small_free_scan_madvise_free(rack_t *rack, magazine_t *depot_ptr, region_t r);

MALLOC_NOEXPORT
kern_return_t
small_in_use_enumerator(task_t task, void *context, unsigned type_mask, szone_t *szone, memory_reader_t reader,
		vm_range_recorder_t recorder);

MALLOC_NOEXPORT
void *
small_malloc_should_clear(rack_t *rack, msize_t msize, boolean_t cleared_requested);

MALLOC_NOEXPORT
void *
small_memalign(szone_t *szone, size_t alignment, size_t size, size_t span);

MALLOC_NOEXPORT
boolean_t
small_claimed_address(rack_t *rack, void *ptr);

MALLOC_NOEXPORT
void *
small_try_shrink_in_place(rack_t *rack, void *ptr, size_t old_size, size_t new_good_size);

MALLOC_NOEXPORT
boolean_t
small_try_realloc_in_place(rack_t *rack, void *ptr, size_t old_size, size_t new_size);

MALLOC_NOEXPORT
void
free_small(rack_t *rack, void *ptr, region_t small_region, size_t known_size);

MALLOC_NOEXPORT
size_t
small_size(rack_t *rack, const void *ptr);

MALLOC_NOEXPORT
void
print_small_free_list(rack_t *rack);

MALLOC_NOEXPORT
void
print_small_region(szone_t *szone, boolean_t verbose, region_t region, size_t bytes_at_start, size_t bytes_at_end);

// MARK: large region allocator functions

MALLOC_NOEXPORT
void
free_large(szone_t *szone, void *ptr);

MALLOC_NOEXPORT
void
large_entries_free_no_lock(szone_t *szone, large_entry_t *entries, unsigned num, vm_range_t *range_to_deallocate);

MALLOC_NOEXPORT
large_entry_t *
large_entry_for_pointer_no_lock(szone_t *szone, const void *ptr);

MALLOC_NOEXPORT
kern_return_t
large_in_use_enumerator(task_t task, void *context, unsigned type_mask, vm_address_t large_entries_address, unsigned num_entries,
		memory_reader_t reader, vm_range_recorder_t recorder);

MALLOC_NOEXPORT
int
large_try_realloc_in_place(szone_t *szone, void *ptr, size_t old_size, size_t new_size);

MALLOC_NOEXPORT
void *
large_try_shrink_in_place(szone_t *szone, void *ptr, size_t old_size, size_t new_good_size);

MALLOC_NOEXPORT
void *
large_malloc(szone_t *szone, size_t num_kernel_pages, unsigned char alignment, boolean_t cleared_requested);

MALLOC_NOEXPORT
boolean_t
large_claimed_address(szone_t *szone, void *ptr);

MALLOC_NOEXPORT
void *
szone_malloc_should_clear(szone_t *szone, size_t size, boolean_t cleared_requested);

// MARK: stack logging lite functionality

#define MALLOC_STOCK_LOGGING_LITE_ZONE_NAME "MallocStackLoggingLiteZone"

// These enable/disable stack logging lite for malloc allocations, not VM-only lite mode
MALLOC_NOEXPORT
void
enable_stack_logging_lite();

MALLOC_NOEXPORT
void
disable_stack_logging_lite();

MALLOC_NOEXPORT
malloc_zone_t *
create_stack_logging_lite_zone(size_t initial_size, malloc_zone_t *helper_zone, unsigned debug_flags);

#endif // __MAGAZINE_MALLOC_H
