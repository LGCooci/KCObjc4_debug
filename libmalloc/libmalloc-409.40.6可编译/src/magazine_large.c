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

#include "internal.h"

static large_entry_t *large_entries_grow_no_lock(szone_t *szone, vm_range_t *range_to_deallocate);

void
large_debug_print(task_t task, unsigned level, vm_address_t zone_address,
		memory_reader_t reader, print_task_printer_t printer)
{
	szone_t *mapped_szone;
	if (reader(task, zone_address, sizeof(szone_t), (void **)&mapped_szone)) {
		printer("Failed to read szone structure\n");
		return;
	}

	unsigned index;
	large_entry_t *range;
	_SIMPLE_STRING b = _simple_salloc();

	if (b) {
		large_entry_t *mapped_large_entries;
		if (reader(task, (vm_address_t)mapped_szone->large_entries,
				mapped_szone->num_large_entries * sizeof(large_entry_t),
				(void **)&mapped_large_entries)) {
			printer("Failed to read large entries\n");
			return;
		}

		_simple_sprintf(b, "Large allocator active blocks - total %y:\n",
				mapped_szone->num_bytes_in_large_objects);
		for (index = 0, range = mapped_large_entries;
				index < mapped_szone->num_large_entries; index++, range++) {
			if (range->address) {
				_simple_sprintf(b, "   Slot %5d: %p, size %y", index,
						(void *)range->address, range->size);
#if CONFIG_DEFERRED_RECLAIM
				_simple_sprintf(b, "%s\n",
						((range->size + 2 * large_vm_page_quanta_size <= UINT32_MAX &&
						mvm_reclaim_is_available(range->reclaim_index))
						? "" : ", kernel reclaimed" ));
#else
				_simple_sprintf(b, "%s\n",
						(range->did_madvise_reusable ? ", madvised" : ""));
#endif // CONFIG_DEFERRED_RECLAIM
			}
		}

#if CONFIG_LARGE_CACHE
		if (large_cache_enabled) {
			_simple_sprintf(b, "\nLarge allocator death row cache, %d entries\n"
					"\tMax cached size:\t%y\n",
					mapped_szone->large_cache_depth,
					(uint64_t)mapped_szone->large_cache_entry_limit);
			_simple_sprintf(b, "\tCurrent size:\t\t%y\n\tReserve size:\t\t%y\n"
					"\tReserve limit:\t\t%y\n",
					mapped_szone->large_entry_cache_bytes,
					mapped_szone->large_entry_cache_reserve_bytes,
					mapped_szone->large_entry_cache_reserve_limit);
			for (index = 0, range = mapped_szone->large_entry_cache;
					index < mapped_szone->large_cache_depth; index++, range++) {
				_simple_sprintf(b, "   Slot %5d: %p, size %y", index,
						(void *)range->address, range->size);
				char *age = "";
				if (index == mapped_szone->large_entry_cache_newest) {
					age = "[newest]";
				} else if (index == mapped_szone->large_entry_cache_oldest) {
					age = "[oldest]";
				}
#if CONFIG_DEFERRED_RECLAIM
				_simple_sprintf(b, "%s\n",
						((range->size + 2 * large_vm_page_quanta_size <= UINT32_MAX &&
						mvm_reclaim_is_available(range->reclaim_index))
						? "" :", kernel reclaimed"));
#else
				_simple_sprintf(b, "%s\n",
						(range->did_madvise_reusable ? ", madvised" : ""));
#endif // CONFIG_DEFERRED_RECLAIM
			}
			_simple_sprintf(b, "\n");
		}
		else
#endif 	// CONFIG_LARGE_CACHE
		{
			_simple_sprintf(b, "Large allocator death row cache not configured\n");
		}
		printer("%s\n", _simple_string(b));
		_simple_sfree(b);
	}
}

#if DEBUG_MALLOC
static void
large_debug_print_self(szone_t *szone, boolean_t verbose)
{
	large_debug_print(mach_task_self(), verbose ? MALLOC_VERBOSE_PRINT_LEVEL : 0,
			(vm_address_t)szone, _malloc_default_reader, malloc_report_simple);
}
#endif // DEBUG_MALLOC

/*
 * Scan the hash ring looking for an entry containing a given pointer.
 */
static large_entry_t *
large_entry_containing_pointer_no_lock(szone_t *szone, const void *ptr)
{
	// result only valid with lock held
	unsigned num_large_entries = szone->num_large_entries;
	unsigned hash_index;
	unsigned index;
	large_entry_t *range;

	if (!num_large_entries) {
		return NULL;
	}

	hash_index = ((uintptr_t)ptr >> vm_page_quanta_shift) % num_large_entries;
	index = hash_index;

	do {
		range = szone->large_entries + index;
		if (range->address == (vm_address_t)ptr) {
			return range;
		} else if ((vm_address_t)ptr >= range->address
				&& (vm_address_t)ptr < range->address + range->size) {
			return range;
		}

		// Since we may be looking for an inner pointer, we might not get an
		// exact match on the address, so we need to scan further and to skip
		// over empty entries. It will usually be faster to scan backwards.
		index = index == 0 ? num_large_entries - 1 : index - 1;
	} while (index != hash_index);

	return NULL;
}

/*
 * Scan the hash ring looking for an entry for the given pointer.
 */
large_entry_t *
large_entry_for_pointer_no_lock(szone_t *szone, const void *ptr)
{
	// result only valid with lock held
	unsigned num_large_entries = szone->num_large_entries;
	unsigned hash_index;
	unsigned index;
	large_entry_t *range;

	if (!num_large_entries) {
		return NULL;
	}

	hash_index = ((uintptr_t)ptr >> vm_page_quanta_shift) % num_large_entries;
	index = hash_index;
#if DEBUG_MALLOC
	large_entry_t *found = NULL;
#endif /* DEBUG_MALLOC */

	do {
		range = szone->large_entries + index;
		if (range->address == (vm_address_t)ptr) {
#if DEBUG_MALLOC
			if (found != NULL) {
				malloc_zone_error(szone->debug_flags, true,
						"Duplicate entry in large table %p!\n", ptr);
			}
			found = range;
#else
			return range;
#endif /* DEBUG_MALLOC */
		}
		if (0 == range->address) {
			break;
		}
		index++;
		if (index == num_large_entries) {
			index = 0;
		}
	} while (index != hash_index);

#if DEBUG_MALLOC
	return found;
#else
	return NULL;
#endif /* DEBUG_MALLOC */
}

static void
large_entry_insert_no_lock(szone_t *szone, large_entry_t range)
{
	unsigned num_large_entries = szone->num_large_entries;
	unsigned hash_index = (((uintptr_t)(range.address)) >> vm_page_quanta_shift) % num_large_entries;
	unsigned index = hash_index;
	large_entry_t *entry;

	// assert(szone->num_large_objects_in_use < szone->num_large_entries); /* must be called with room to spare */

	do {
		entry = szone->large_entries + index;
		if (0 == entry->address) {
			*entry = range;
			return; // end of chain
		}
		index++;
		if (index == num_large_entries) {
			index = 0;
		}
	} while (index != hash_index);

	// assert(0); /* must not fallthrough! */
}

/*
 * Insert the entry into the hash-table
 * growing it if needed. Caller should hold the szone lock.
 * Returns false if unable to allocate memory to grow hash table. Otherwise returns true.
 */
static bool
large_entry_grow_and_insert_no_lock(szone_t *szone, vm_address_t addr, vm_size_t size,
		vm_range_t *range_to_deallocate)
{
	bool should_grow = (szone->num_large_objects_in_use + 1) * 4 > szone->num_large_entries;
	if (should_grow) {
		// density of hash table too high; grow table
		// we do that under lock to avoid a race
		large_entry_t *entries = large_entries_grow_no_lock(szone, range_to_deallocate);
		if (entries == NULL) {
			return false;
		}
	}

	large_entry_t large_entry;
	large_entry.address = addr;
	large_entry.size = size;
#if CONFIG_DEFERRED_RECLAIM
	large_entry.reclaim_index = VM_RECLAIM_INDEX_NULL;
#else
	large_entry.did_madvise_reusable = FALSE;
#endif // CONFIG_DEFERRED_RECLAIM
	large_entry_insert_no_lock(szone, large_entry);

	szone->num_large_objects_in_use++;
	szone->num_bytes_in_large_objects += size;
	return true;
}

// FIXME: can't we simply swap the (now empty) entry with the last entry on the collision chain for this hash slot?
static MALLOC_INLINE void
large_entries_rehash_after_entry_no_lock(szone_t *szone, large_entry_t *entry)
{
	unsigned num_large_entries = szone->num_large_entries;
	uintptr_t hash_index = entry - szone->large_entries;
	uintptr_t index = hash_index;
	large_entry_t range;

	// assert(entry->address == 0) /* caller must have cleared *entry */

	do {
		index++;
		if (index == num_large_entries) {
			index = 0;
		}
		range = szone->large_entries[index];
		if (0 == range.address) {
			return;
		}
		szone->large_entries[index].address = (vm_address_t)0;
		szone->large_entries[index].size = 0;
#if CONFIG_DEFERRED_RECLAIM
		szone->large_entries[index].reclaim_index = VM_RECLAIM_INDEX_NULL;
#else
		szone->large_entries[index].did_madvise_reusable = FALSE;
#endif // CONFIG_DEFERRED_RECLAIM
		large_entry_insert_no_lock(szone, range); // this will reinsert in the
		// proper place
	} while (index != hash_index);

	// assert(0); /* since entry->address == 0, must not fallthrough! */
}

// FIXME: num should probably be a size_t, since you can theoretically allocate
// more than 2^32-1 large_threshold objects in 64 bit.
static MALLOC_INLINE large_entry_t *
large_entries_alloc_no_lock(szone_t *szone, unsigned num)
{
	size_t size = num * sizeof(large_entry_t);

	// Note that we allocate memory (via a system call) under a spin lock
	// That is certainly evil, however it's very rare in the lifetime of a process
	// The alternative would slow down the normal case
	unsigned flags = MALLOC_APPLY_LARGE_ASLR(szone->debug_flags & (DISABLE_ASLR | DISABLE_LARGE_ASLR));
	return mvm_allocate_pages(round_large_page_quanta(size), 0, flags, VM_MEMORY_MALLOC_LARGE);
}

void
large_entries_free_no_lock(szone_t *szone, large_entry_t *entries, unsigned num, vm_range_t *range_to_deallocate)
{
	size_t size = num * sizeof(large_entry_t);

	range_to_deallocate->address = (vm_address_t)entries;
	range_to_deallocate->size = round_large_page_quanta(size);
}

static large_entry_t *
large_entries_grow_no_lock(szone_t *szone, vm_range_t *range_to_deallocate)
{
	// sets range_to_deallocate
	unsigned old_num_entries = szone->num_large_entries;
	large_entry_t *old_entries = szone->large_entries;
	// always an odd number for good hashing
	unsigned new_num_entries =
	(old_num_entries) ? old_num_entries * 2 + 1 : (unsigned)((large_vm_page_quanta_size / sizeof(large_entry_t)) - 1);
	large_entry_t *new_entries = large_entries_alloc_no_lock(szone, new_num_entries);
	unsigned index = old_num_entries;
	large_entry_t oldRange;

	// if the allocation of new entries failed, bail
	if (new_entries == NULL) {
		return NULL;
	}

	szone->num_large_entries = new_num_entries;
	szone->large_entries = new_entries;

	/* rehash entries into the new list */
	while (index--) {
		oldRange = old_entries[index];
		if (oldRange.address) {
			large_entry_insert_no_lock(szone, oldRange);
		}
	}

	if (old_entries) {
		large_entries_free_no_lock(szone, old_entries, old_num_entries, range_to_deallocate);
	} else {
		range_to_deallocate->address = (vm_address_t)0;
		range_to_deallocate->size = 0;
	}

	return new_entries;
}

// frees the specific entry in the size table
// returns a range to truly deallocate
static vm_range_t
large_entry_free_no_lock(szone_t *szone, large_entry_t *entry)
{
	vm_range_t range;

	MALLOC_TRACE(TRACE_large_free, (uintptr_t)szone, (uintptr_t)entry->address, entry->size, 0);

	range.address = entry->address;
	range.size = entry->size;

	if (szone->debug_flags & MALLOC_ADD_GUARD_PAGE_FLAGS) {
		mvm_protect((void *)range.address, range.size, PROT_READ | PROT_WRITE, szone->debug_flags);
		range.address -= large_vm_page_quanta_size;
		range.size += 2 * large_vm_page_quanta_size;
	}

	entry->address = 0;
	entry->size = 0;
#if CONFIG_DEFERRED_RECLAIM
	entry->reclaim_index = VM_RECLAIM_INDEX_NULL;
#else
	entry->did_madvise_reusable = FALSE;
#endif // CONFIG_DEFERRED_RECLAIM
	large_entries_rehash_after_entry_no_lock(szone, entry);

#if DEBUG_MALLOC
	if (large_entry_for_pointer_no_lock(szone, (void *)range.address)) {
		large_debug_print_self(szone, 1);
		malloc_report(ASL_LEVEL_ERR, "*** freed entry %p still in use; num_large_entries=%d\n", (void *)range.address, szone->num_large_entries);
	}
#endif
	return range;
}

kern_return_t
large_in_use_enumerator(task_t task,
						void *context,
						unsigned type_mask,
						vm_address_t large_entries_address,
						unsigned num_entries,
						memory_reader_t reader,
						vm_range_recorder_t recorder)
{
	unsigned index = 0;
	vm_range_t buffer[MAX_RECORDER_BUFFER];
	unsigned count = 0;
	large_entry_t *entries;
	kern_return_t err;
	vm_range_t range;
	large_entry_t entry;

	err = reader(task, large_entries_address, sizeof(large_entry_t) * num_entries, (void **)&entries);
	if (err) {
		return err;
	}

	index = num_entries;
	if (type_mask & MALLOC_ADMIN_REGION_RANGE_TYPE) {
		range.address = large_entries_address;
		range.size = round_large_page_quanta(num_entries * sizeof(large_entry_t));
		recorder(task, context, MALLOC_ADMIN_REGION_RANGE_TYPE, &range, 1);
	}
	if (type_mask & (MALLOC_PTR_IN_USE_RANGE_TYPE | MALLOC_PTR_REGION_RANGE_TYPE)) {
		while (index--) {
			entry = entries[index];
			if (entry.address) {
				range.address = entry.address;
				range.size = entry.size;
				buffer[count++] = range;
				if (count >= MAX_RECORDER_BUFFER) {
					recorder(task, context, MALLOC_PTR_IN_USE_RANGE_TYPE | MALLOC_PTR_REGION_RANGE_TYPE, buffer, count);
					count = 0;
				}
			}
		}
	}
	if (count) {
		recorder(task, context, MALLOC_PTR_IN_USE_RANGE_TYPE | MALLOC_PTR_REGION_RANGE_TYPE, buffer, count);
	}
	return 0;
}

#if CONFIG_LARGE_CACHE
/*
 * Remove the entry at idx from the death row cache.
 * Does not operate on the entry itself.
 * Caller must hold the szone lock.
 *
 * Returns the oldest entry idx after the removed entry (or -1 if the oldest entry was removed)
 * so that the caller can iterate through the buffer while removing entries.
 */
static int
remove_from_death_row_no_lock(szone_t *szone, int idx)
{
	int i, next_idx = -1;
	// Compact live ring to fill entry now vacated at large_entry_cache[best]
	// while preserving time-order
	if (szone->large_entry_cache_oldest < szone->large_entry_cache_newest) {
		// Ring hasn't wrapped. Fill in from right.
		for (i = idx; i < szone->large_entry_cache_newest; ++i) {
			szone->large_entry_cache[i] = szone->large_entry_cache[i + 1];
		}

		if (idx == szone->large_entry_cache_oldest) {
			next_idx = -1;
		} else {
			next_idx = idx - 1;
		}
		szone->large_entry_cache_newest--; // Pull in right endpoint.
	} else if (szone->large_entry_cache_newest < szone->large_entry_cache_oldest) {
		// Ring has wrapped. Arrange to fill in from the contiguous side.
		if (idx <= szone->large_entry_cache_newest) {
			// Fill from right.
			for (i = idx; i < szone->large_entry_cache_newest; ++i) {
				szone->large_entry_cache[i] = szone->large_entry_cache[i + 1];
			}

			if (0 < szone->large_entry_cache_newest) {
				szone->large_entry_cache_newest--;
			} else {
				szone->large_entry_cache_newest = szone->large_cache_depth - 1;
			}
			if (idx == 0) {
				next_idx = szone->large_cache_depth - 1;
			} else {
				next_idx = idx - 1;
			}
		} else {
			// Fill from left.
			for (i = idx; i > szone->large_entry_cache_oldest; --i) {
				szone->large_entry_cache[i] = szone->large_entry_cache[i - 1];
			}

			if (idx == szone->large_entry_cache_oldest) {
				next_idx = -1;
			} else {
				next_idx = idx;
			}
			if (szone->large_entry_cache_oldest < szone->large_cache_depth - 1) {
				szone->large_entry_cache_oldest++;
			} else {
				szone->large_entry_cache_oldest = 0;
			}

		}
	} else {
		// By trichotomy, large_entry_cache_newest == large_entry_cache_oldest.
		// That implies best == large_entry_cache_newest == large_entry_cache_oldest
		// and the ring is now empty.
		szone->large_entry_cache[idx].address = 0;
		szone->large_entry_cache[idx].size = 0;
#if CONFIG_DEFERRED_RECLAIM
		szone->large_entry_cache[idx].reclaim_index = VM_RECLAIM_INDEX_NULL;
#else
		szone->large_entry_cache[idx].did_madvise_reusable = FALSE;
#endif // CONFIG_DEFERRED_RECLAIM 
		next_idx = -1;
	}

	return next_idx;
}

/*
 * Look for the best fit in the death row cache.
 * Returns an entry with address NULL iff there is no suitable
 * entry in the cache.
 */
static large_entry_t
large_malloc_best_fit_in_cache(szone_t *szone, size_t size, unsigned char alignment)
{
	int best = -1, idx = szone->large_entry_cache_newest, stop_idx = szone->large_entry_cache_oldest;
	size_t best_size = SIZE_T_MAX;
	large_entry_t entry;
	memset(&entry, 0, sizeof(entry));
	// Scan large_entry_cache for best fit, starting with most recent entry
	while (1) {
		size_t this_size = szone->large_entry_cache[idx].size;
		vm_address_t addr = szone->large_entry_cache[idx].address;

		if (0 == alignment || 0 == (((uintptr_t)addr) & (((uintptr_t)1 << alignment) - 1))) {
			if (size == this_size || (size < this_size && this_size < best_size)) {
#if CONFIG_DEFERRED_RECLAIM
				uint64_t reclaim_index = szone->large_entry_cache[idx].reclaim_index;
				if (best_size + 2 * large_vm_page_quanta_size <= UINT32_MAX &&
						!mvm_reclaim_is_available(reclaim_index)) {
					// Kernel has already reclaimed this entry or
					// is in the process of trying to reclaim it.
					// Remove it from death row & keep looking
					idx = remove_from_death_row_no_lock(szone, idx);
					stop_idx = szone->large_entry_cache_oldest;
					if (idx == -1) {
						// We've looked at all entries in the cache
						break;
					} else {
						continue;
					}
				}
#endif // CONFIG_DEFERRED_RECLAIM
				best = idx;
				best_size = this_size;
				if (size == this_size) {
					// Perfect fit. No need to keep looking.
					break;
				}
			}
		}

		if (idx == stop_idx) { // exhausted live ring?
			break;
		}

		if (idx) {
			idx--; // bump idx down
		} else {
			idx = szone->large_cache_depth - 1; // wrap idx
		}
	}

	if (best == -1 || (best_size - size) >= size) { // limit fragmentation to 50%
		return entry;
	}

	entry = szone->large_entry_cache[best];

	remove_from_death_row_no_lock(szone, best);

	return entry;
}

/*
 * Attempt to handle the allocation from the death-row cache
 * Caller should not hold the szone lock & it will be unlocked on return.
 * Returns NULL if unable to satisfy the allocation from the death-row cache.
 */
static void *
large_malloc_from_cache(szone_t *szone, size_t size, unsigned char alignment, boolean_t cleared_requested)
{
	SZONE_LOCK(szone);

	bool was_madvised_reusable;
	large_entry_t entry;

	while (true) {
		entry = large_malloc_best_fit_in_cache(szone, size, alignment);
		if (entry.address == (vm_address_t)NULL) {
			// The cache does not contain an entry that we can use.
			SZONE_UNLOCK(szone);
			return NULL;
		} else {
#if CONFIG_DEFERRED_RECLAIM
			if (entry.size + 2 * large_vm_page_quanta_size <= UINT32_MAX &&
					!mvm_reclaim_mark_used(entry.reclaim_index, entry.address,
					(uint32_t) entry.size, szone->debug_flags)) {
				// Entry has been reclaimed by the kernel since we put it in the death row cache
				// large_malloc_best_fit_in_cache already removed it from the cache.
				// Let's search the cache again to see if there's another entry we can use.
				// Note that we never dropped the SZONE lock so this search is bounded by the size
				// of the cache and mvm_reclaim_mark_used synchronized with the kernel so
				// subsequent calls to large_malloc_best_fit_in_cache
				// will clear out any entries that were reclaimed before this one.
				continue;
			}
#endif // CONFIG_DEFERRED_RECLAIM
			/* Got an entry */
			break;
		}
	}

	vm_range_t range_to_deallocate;
	range_to_deallocate.size = 0;
	range_to_deallocate.address = 0;
	bool success = large_entry_grow_and_insert_no_lock(szone, entry.address, entry.size,
			&range_to_deallocate);

	if (!success) {
		SZONE_UNLOCK(szone);
		return NULL;
	}
#if CONFIG_DEFERRED_RECLAIM
	was_madvised_reusable = true;
#else
	was_madvised_reusable = entry.did_madvise_reusable;
#endif // CONFIG_DEFERRED_RECLAIM
	if (!was_madvised_reusable) {
		szone->large_entry_cache_reserve_bytes -= entry.size;
	}

	szone->large_entry_cache_bytes -= entry.size;

	if (szone->flotsam_enabled && szone->large_entry_cache_bytes < SZONE_FLOTSAM_THRESHOLD_LOW) {
		szone->flotsam_enabled = FALSE;
	}

	SZONE_UNLOCK(szone);

	if (range_to_deallocate.size) {
		// we deallocate outside the lock
		mvm_deallocate_pages((void *)range_to_deallocate.address, range_to_deallocate.size, 0);
	}

	if (cleared_requested) {
		memset((void *) entry.address, 0, size);
	}

	return (void *)entry.address;
}
#endif /* CONFIG_LARGE_CACHE */

void *
large_malloc(szone_t *szone, size_t num_kernel_pages, unsigned char alignment, boolean_t cleared_requested)
{
	void *addr;
	vm_range_t range_to_deallocate;
	size_t size;

	MALLOC_TRACE(TRACE_large_malloc, (uintptr_t)szone, num_kernel_pages, alignment, cleared_requested);

	if (!num_kernel_pages) {
		num_kernel_pages = 1; // minimal allocation size for this szone
	}
	size = (size_t)num_kernel_pages << large_vm_page_quanta_shift;
	range_to_deallocate.size = 0;
	range_to_deallocate.address = 0;

#if CONFIG_LARGE_CACHE
	// Look for a large_entry_t on the death-row cache?
	if (large_cache_enabled && size <= szone->large_cache_entry_limit) {
		addr = large_malloc_from_cache(szone, size, alignment, cleared_requested);
		if (addr != NULL) {
			return addr;
		}
	}
#endif /* CONFIG_LARGE_CACHE */

	// NOTE: we do not use MALLOC_FIX_GUARD_PAGE_FLAGS(szone->debug_flags) here
	// because we want to always add either no guard page or both guard pages.
	addr = mvm_allocate_pages(size, alignment, MALLOC_APPLY_LARGE_ASLR(szone->debug_flags), VM_MEMORY_MALLOC_LARGE);
	if (addr == NULL) {
		return NULL;
	}

	SZONE_LOCK(szone);
	bool success = large_entry_grow_and_insert_no_lock(szone, (vm_address_t) addr, (vm_size_t) size,
			&range_to_deallocate);
	SZONE_UNLOCK(szone);
	if (!success) {
		return NULL;
	}

	if (range_to_deallocate.size) {
		// we deallocate outside the lock
		mvm_deallocate_pages((void *)range_to_deallocate.address, range_to_deallocate.size, 0);
	}
	return addr;
}

bool
free_large(szone_t *szone, void *ptr, bool try)
{
	// We have established ptr is page-aligned and neither tiny nor small
	large_entry_t *entry;
	vm_range_t vm_range_to_deallocate;
	vm_range_to_deallocate.size = 0;
	vm_range_to_deallocate.address = 0;

	SZONE_LOCK(szone);
	entry = large_entry_for_pointer_no_lock(szone, ptr);
	if (entry) {
#if CONFIG_LARGE_CACHE
		if (large_cache_enabled &&
				entry->size <= szone->large_cache_entry_limit
#if !CONFIG_DEFERRED_RECLAIM
				&& -1 != madvise((void *)(entry->address), entry->size, MADV_CAN_REUSE)
#endif // CONFIG_DEFERRED_RECLAIM
				) { // Put the large_entry_t on the death-row cache?
				int idx = szone->large_entry_cache_newest, stop_idx = szone->large_entry_cache_oldest;
				// Make a local copy, we'll free the entry from the lookup table
				// before dropping the lock.
				large_entry_t this_entry = *entry;
#if !CONFIG_DEFERRED_RECLAIM
				boolean_t should_madvise = szone->large_entry_cache_reserve_bytes +
						this_entry.size > szone->large_entry_cache_reserve_limit;
#endif // !CONFIG_DEFERRED_RECLAIM
				boolean_t reusable = TRUE;

				// Already freed?
				// [Note that repeated entries in death-row risk vending the same entry subsequently
				// to two different malloc() calls. By checking here the (illegal) double free
				// is accommodated, matching the behavior of the previous implementation.]
				while (1) { // Scan large_entry_cache starting with most recent entry
					vm_size_t curr_size = szone->large_entry_cache[idx].size;
					vm_address_t addr = szone->large_entry_cache[idx].address;
#if CONFIG_DEFERRED_RECLAIM
					uint64_t reclaim_index = szone->large_entry_cache[idx].reclaim_index;
					if (curr_size + 2 * large_vm_page_quanta_size <= UINT32_MAX &&
							!mvm_reclaim_is_available(reclaim_index)) {
						int next_idx = idx;

						// Entry has been reclaimed
						// Remove it from the cache

						next_idx = remove_from_death_row_no_lock(szone, idx);
						stop_idx = szone->large_entry_cache_oldest;
						if (next_idx == -1) {
							// Ring buffer is now empty
							break;
						}

						continue;
					}
#endif // CONFIG_DEFERRED_RECLAIM
					if (addr == entry->address) {
#if CONFIG_DEFERRED_RECLAIM
						if (curr_size + 2 * large_vm_page_quanta_size <= UINT32_MAX) {
							// mvm_reclaim_is_available doesn't actually synchronize with the kernel,
							// so in order to confidently say this was a double free
							// we need to make sure the entry was not reclaimed.
							if (!mvm_reclaim_mark_used(reclaim_index, addr, (uint32_t) curr_size, szone->debug_flags)) {
								// This entry has been reclaimed, so it's not a double-free. continue
								break;
							}
							// This is a double free, but we just took the entry
							// out of the reclaim buffer. Put it back.
							szone->large_entry_cache[idx].reclaim_index =
							    mvm_reclaim_mark_free(addr, (uint32_t) curr_size, szone->debug_flags);
							reclaim_index = szone->large_entry_cache[idx].reclaim_index;
						}
#endif // CONFIG_DEFERRED_RECLAIM
						malloc_zone_error(szone->debug_flags, true, "pointer %p being freed already on death-row\n", ptr);
						SZONE_UNLOCK(szone);
						return true;
					}

					if (idx == stop_idx) { // exhausted live ring?
						break;
					}

					if (idx) {
						idx--; // bump idx down
					} else {
						idx = szone->large_cache_depth - 1; // wrap idx
					}
				}

				vm_range_to_deallocate = large_entry_free_no_lock(szone, entry);
				entry = NULL;

				SZONE_UNLOCK(szone);

				if (szone->debug_flags & MALLOC_PURGEABLE) { // Are we a purgable zone?
					int state = VM_PURGABLE_NONVOLATILE;			  // restore to default condition

					if (KERN_SUCCESS != vm_purgable_control(mach_task_self(), this_entry.address, VM_PURGABLE_SET_STATE, &state)) {
						malloc_report(ASL_LEVEL_ERR, "*** can't vm_purgable_control(..., VM_PURGABLE_SET_STATE) for large freed block at %p\n",
									  (void *)this_entry.address);
						reusable = FALSE;
					}
				}

				if (szone->large_legacy_reset_mprotect) { // Linked for Leopard?
					// Accomodate Leopard apps that (illegally) mprotect() their own guard pages on large malloc'd allocations
					int err = mprotect((void *)(this_entry.address), this_entry.size, PROT_READ | PROT_WRITE);
					if (err) {
						malloc_report(ASL_LEVEL_ERR, "*** can't reset protection for large freed block at %p\n", (void *)this_entry.address);
						reusable = FALSE;
					}
				}

				// madvise(..., MADV_REUSABLE) death-row arrivals if hoarding would exceed large_entry_cache_reserve_limit

#if CONFIG_DEFERRED_RECLAIM
				if (reusable) {
					if ((szone->debug_flags & MALLOC_DO_SCRIBBLE)) {
						memset((void *)(this_entry.address), SCRUBBLE_BYTE, this_entry.size);
					}
					// Only put this in the reclaim buffer if its size (plus any guard pages)
					// can fit in a uint32_t.
					if (this_entry.size + 2 * large_vm_page_quanta_size > UINT32_MAX) {
						reusable = false;
					}
					this_entry.reclaim_index = mvm_reclaim_mark_free(this_entry.address,
					    (uint32_t) this_entry.size, szone->debug_flags);
					// NB: At this point this_entry.address could be reclaimed
				}
#else
				if (should_madvise) {
					// Issue madvise to avoid paging out the dirtied free()'d pages in "entry"
					MAGMALLOC_MADVFREEREGION((void *)szone, (void *)0,
							(void *)(this_entry.address), (int)this_entry.size); // DTrace USDT Probe

					// Ok to do this madvise on embedded because we won't call MADV_FREE_REUSABLE on a large
					// cache block twice without MADV_FREE_REUSE in between.

					if (-1 == madvise((void *)(this_entry.address), this_entry.size, MADV_FREE_REUSABLE)) {
						/* -1 return: VM map entry change makes this unfit for reuse. */
#if DEBUG_MADVISE
						malloc_zone_error(szone->debug_flags, false,
									"free_large madvise(..., MADV_FREE_REUSABLE) failed for %p, length=%d\n",
									(void *)this_entry.address, this_entry.size);
#endif
						reusable = FALSE;
					}
				}
#endif // CONFIG_DEFERRED_RECLAIM

				SZONE_LOCK(szone);

				szone->num_large_objects_in_use--;
				szone->num_bytes_in_large_objects -= this_entry.size;

				// Add "this_entry" to death-row ring
				if (reusable) {
					int idx = szone->large_entry_cache_newest; // Most recently occupied
					vm_address_t addr;
					size_t adjsize;
#if CONFIG_DEFERRED_RECLAIM
					uint64_t old_reclaim_index;
#endif // CONFIG_DEFERRED_RECLAIM

					if (szone->large_entry_cache_newest == szone->large_entry_cache_oldest &&
						0 == szone->large_entry_cache[idx].address) {
						// Ring is empty, idx is good as it stands
						addr = 0;
						adjsize = 0;
					} else {
						// Extend the queue to the "right" by bumping up large_entry_cache_newest
						if (idx == szone->large_cache_depth - 1) {
							idx = 0; // Wrap index
						} else {
							idx++; // Bump index
						}
						if (idx == szone->large_entry_cache_oldest) { // Fully occupied
							// Drop this entry from the cache and deallocate the VM
							addr = szone->large_entry_cache[idx].address;
							adjsize = szone->large_entry_cache[idx].size;
							szone->large_entry_cache_bytes -= adjsize;
#if CONFIG_DEFERRED_RECLAIM
							old_reclaim_index = szone->large_entry_cache[idx].reclaim_index;
#else
							if (!szone->large_entry_cache[idx].did_madvise_reusable) {
								szone->large_entry_cache_reserve_bytes -= adjsize;
							}
#endif // CONFIG_DEFERRED_RECLAIM
						} else {
							// Using an unoccupied cache slot
							addr = 0;
							adjsize = 0;
						}
					}

#if !CONFIG_DEFERRED_RECLAIM
					if ((szone->debug_flags & MALLOC_DO_SCRIBBLE)) {
						memset((void *)(this_entry.address), should_madvise ?
								SCRUBBLE_BYTE : SCRABBLE_BYTE, this_entry.size);
					}
					this_entry.did_madvise_reusable = should_madvise; // Was madvise()'d above?
					if (!should_madvise) {
						// Entered on death-row without madvise() => up the hoard total
						szone->large_entry_cache_reserve_bytes += this_entry.size;
					}
#endif // !CONFIG_DEFERRED_RECLAIM

					szone->large_entry_cache_bytes += this_entry.size;

					if (!szone->flotsam_enabled && szone->large_entry_cache_bytes > SZONE_FLOTSAM_THRESHOLD_HIGH) {
						szone->flotsam_enabled = TRUE;
					}

					szone->large_entry_cache[idx] = this_entry;
					szone->large_entry_cache_newest = idx;

					if (0 == addr) {
						SZONE_UNLOCK(szone);
						return true;
					}

					// Fall through to drop large_entry_cache_oldest from the cache,
					// and then deallocate its pages.

					// Trim the queue on the "left" by bumping up large_entry_cache_oldest
					if (szone->large_entry_cache_oldest == szone->large_cache_depth - 1) {
						szone->large_entry_cache_oldest = 0;
					} else {
						szone->large_entry_cache_oldest++;
					}

					// we deallocate_pages, including guard pages, outside the lock
					SZONE_UNLOCK(szone);

#if CONFIG_DEFERRED_RECLAIM
					// Need to take ownership of the allocation before trying to deallocate it.
					if (adjsize + 2 * large_vm_page_quanta_size <= UINT32_MAX &&
							mvm_reclaim_mark_used(old_reclaim_index, addr,
							(uint32_t) adjsize, szone->debug_flags)) {
						mvm_deallocate_pages((void *)addr, (size_t)adjsize, szone->debug_flags);
					}
#else
					mvm_deallocate_pages((void *)addr, (size_t)adjsize, 0);
#endif // CONFIG_DEFERRED_RECLAIM
					return true;
				} else {
					// fall through to deallocate vm_range_to_deallocate
				}
			}
#endif /* CONFIG_LARGE_CACHE */

		if (!vm_range_to_deallocate.address) {
			szone->num_large_objects_in_use--;
			szone->num_bytes_in_large_objects -= entry->size;

			vm_range_to_deallocate = large_entry_free_no_lock(szone, entry);
		}
	} else {
		if (!try) {
#if DEBUG_MALLOC
			large_debug_print_self(szone, 1);
#endif
			malloc_zone_error(szone->debug_flags, true, "pointer %p being freed was not allocated\n", ptr);
		}
		SZONE_UNLOCK(szone);
		return false;
	}
	SZONE_UNLOCK(szone); // we release the lock asap
	CHECK(szone, __PRETTY_FUNCTION__);

	// we deallocate_pages, including guard pages, outside the lock
	if (vm_range_to_deallocate.address) {
#if DEBUG_MALLOC
		// FIXME: large_entry_for_pointer_no_lock() needs the lock held ...
		if (large_entry_for_pointer_no_lock(szone, (void *)vm_range_to_deallocate.address)) {
			large_debug_print_self(szone, 1);
			malloc_report(ASL_LEVEL_ERR, "*** invariant broken: %p still in use num_large_entries=%d\n",
					(void *)vm_range_to_deallocate.address, szone->num_large_entries);
		}
#endif
		mvm_deallocate_pages((void *)vm_range_to_deallocate.address, (size_t)vm_range_to_deallocate.size, 0);
	}

	return true;
}

void *
large_try_shrink_in_place(szone_t *szone, void *ptr, size_t old_size, size_t new_good_size)
{
	size_t shrinkage = old_size - new_good_size;

	if (shrinkage) {
		SZONE_LOCK(szone);
		/* contract existing large entry */
		large_entry_t *large_entry = large_entry_for_pointer_no_lock(szone, ptr);
		if (!large_entry) {
			malloc_zone_error(szone->debug_flags, true, "large entry %p reallocated is not properly in table\n", ptr);
			SZONE_UNLOCK(szone);
			return ptr;
		}

		large_entry->address = (vm_address_t)ptr;
		large_entry->size = new_good_size;
		szone->num_bytes_in_large_objects -= shrinkage;
		boolean_t guarded = szone->debug_flags & MALLOC_ADD_GUARD_PAGE_FLAGS;
		SZONE_UNLOCK(szone); // we release the lock asap

		if (guarded) {
			// Keep the page above the new end of the allocation as the
			// postlude guard page.
			kern_return_t err;
			err = mprotect((void *)((uintptr_t)ptr + new_good_size), large_vm_page_quanta_size, 0);
			if (err) {
				malloc_report(ASL_LEVEL_ERR, "*** can't mvm_protect(0x0) region for new postlude guard page at %p\n",
						  ptr + new_good_size);
			}
			new_good_size += large_vm_page_quanta_size;
			shrinkage -= large_vm_page_quanta_size;
		}

		mvm_deallocate_pages((void *)((uintptr_t)ptr + new_good_size), shrinkage, 0);
	}
	return ptr;
}

int
large_try_realloc_in_place(szone_t *szone, void *ptr, size_t old_size, size_t new_size)
{
	vm_address_t addr = (vm_address_t)ptr + old_size;
	large_entry_t *large_entry;
	kern_return_t err;

	SZONE_LOCK(szone);
	large_entry = large_entry_for_pointer_no_lock(szone, (void *)addr);
	SZONE_UNLOCK(szone);

	if (large_entry) { // check if "addr = ptr + old_size" is already spoken for
		return 0;	  // large pointer already exists in table - extension is not going to work
	}

	new_size = round_large_page_quanta(new_size);
	/*
	 * Ask for allocation at a specific address, and mark as realloc
	 * to request coalescing with previous realloc'ed extensions.
	 */
	err = vm_allocate(mach_task_self(), &addr, new_size - old_size, VM_MAKE_TAG(VM_MEMORY_REALLOC));
	if (err != KERN_SUCCESS) {
		return 0;
	}

	SZONE_LOCK(szone);
	/* extend existing large entry */
	large_entry = large_entry_for_pointer_no_lock(szone, ptr);
	if (!large_entry) {
		malloc_zone_error(szone->debug_flags, true, "large entry %p reallocated is not properly in table\n", ptr);
		SZONE_UNLOCK(szone);
		return 0; // Bail, leaking "addr"
	}
	
	large_entry->address = (vm_address_t)ptr;
	large_entry->size = new_size;
	szone->num_bytes_in_large_objects += new_size - old_size;
	SZONE_UNLOCK(szone); // we release the lock asap
	
	return 1;
}

boolean_t
large_claimed_address(szone_t *szone, void *ptr)
{
	SZONE_LOCK(szone);
	boolean_t result = large_entry_containing_pointer_no_lock(szone,
			(void *)trunc_page((uintptr_t)ptr)) != NULL;
	SZONE_UNLOCK(szone);
	return result;
}

#if CONFIG_LARGE_CACHE
static void
large_clear_cache_locked(szone_t *szone)
{
	szone->large_entry_cache_oldest = szone->large_entry_cache_newest = 0;
	szone->large_entry_cache[0].address = 0x0;
	szone->large_entry_cache[0].size = 0;
	szone->large_entry_cache_bytes = 0;
	szone->large_entry_cache_reserve_bytes = 0;
}

static void
large_deallocate_cache_entry(szone_t *szone, large_entry_t *entry)
{
#if CONFIG_DEFERRED_RECLAIM
	// If we're using deferred reclaim, we have to first take ownership of the entry back
	// out of the reclaim buffer. If we fail to get the entry, then it's already been
	// reclaimed.
	if (entry->size > UINT32_MAX ||
		mvm_reclaim_mark_used(entry->reclaim_index, entry->address,
				(uint32_t) entry->size, szone->debug_flags)) {
		mvm_deallocate_pages((void *)entry->address, entry->size, szone->debug_flags);
	}
#else // CONFIG_DEFERRED_RECLAIM
	mvm_deallocate_pages(entry->address, entry->size, szone->debug_flags);
#endif // CONFIG_DEFERRED_RECLAIM
}

void
large_destroy_cache(szone_t *szone)
{
	SZONE_LOCK(szone);

	// disable any memory pressure responder
	szone->flotsam_enabled = FALSE;
	// stack allocated copy of the death-row cache
	int idx = szone->large_entry_cache_oldest, idx_max = szone->large_entry_cache_newest;
	large_entry_t local_entry_cache[LARGE_ENTRY_CACHE_SIZE_HIGH];

	memcpy((void *)local_entry_cache, (void *)szone->large_entry_cache, sizeof(local_entry_cache));

	large_clear_cache_locked(szone);
	SZONE_UNLOCK(szone);

	// deallocate the death-row cache entries outside the zone lock
	while (idx != idx_max) {
		large_entry_t *entry = &local_entry_cache[idx];

		large_deallocate_cache_entry(szone, entry);
		if (++idx == szone->large_cache_depth) {
			idx = 0;
		}
	}

	if (0 != local_entry_cache[idx].address && 0 != local_entry_cache[idx].size) {
		large_deallocate_cache_entry(szone, &local_entry_cache[idx]);
	}
}

#endif // CONFIG_LARGE_CACHE
