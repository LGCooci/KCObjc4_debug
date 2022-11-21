/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#if CONFIG_QUARANTINE

#pragma mark -
#pragma mark Types and Structures

typedef struct {
	// Malloc zone
	malloc_zone_t malloc_zone;
	malloc_zone_t *wrapped_zone;

	// Configuration
	bool debug;
	bool do_poisoning;
	size_t max_items_in_quarantine; // 0 means unlimited
	size_t max_bytes_in_quarantine; // 0 means unlimited

	// Stacktrace tracking data structures
	struct stacktrace_depo_t *depo;
	struct pointer_map_t *map;

	uint8_t padding[PAGE_MAX_SIZE];

	// Mutable state
	_malloc_lock_s lock;
	struct quarantined_chunk *quarantine_head;
	struct quarantined_chunk *quarantine_tail;
	size_t items_in_quarantine;
	size_t bytes_in_quarantine;
} quarantine_zone_t;

MALLOC_STATIC_ASSERT(__offsetof(quarantine_zone_t, malloc_zone) == 0,
		"quarantine_zone_t instances must be usable as regular zones");
MALLOC_STATIC_ASSERT(__offsetof(quarantine_zone_t, padding) < PAGE_MAX_SIZE,
		"First page is mapped read-only");
MALLOC_STATIC_ASSERT(__offsetof(quarantine_zone_t, lock) >= PAGE_MAX_SIZE,
		"Mutable state is on separate page");
MALLOC_STATIC_ASSERT(sizeof(quarantine_zone_t) < (2 * PAGE_MAX_SIZE),
		"Zone fits on 2 pages");

#define DELEGATE(function, args...) \
	zone->wrapped_zone->function(zone->wrapped_zone, args)

// Lock helpers
static void
init_lock(quarantine_zone_t *zone)
{
	_malloc_lock_init(&zone->lock);
}

static void
lock(quarantine_zone_t *zone)
{
	_malloc_lock_lock(&zone->lock);
}

static void
unlock(quarantine_zone_t *zone)
{
	_malloc_lock_unlock(&zone->lock);
}

static bool
trylock(quarantine_zone_t *zone)
{
	return _malloc_lock_trylock(&zone->lock);
}

// VM allocation/deallocate helpers
static vm_address_t
quarantine_vm_map(size_t size, vm_prot_t protection, int tag)
{
	vm_map_t target = mach_task_self();
	mach_vm_address_t address = 0;
	mach_vm_size_t size_rounded = round_page(size);
	mach_vm_offset_t mask = 0x0;
	int flags = VM_FLAGS_ANYWHERE | VM_MAKE_TAG(tag);
	mem_entry_name_port_t object = MEMORY_OBJECT_NULL;
	memory_object_offset_t offset = 0;
	bool copy = false;
	vm_prot_t cur_protection = protection;
	vm_prot_t max_protection = VM_PROT_READ | VM_PROT_WRITE;
	vm_inherit_t inheritance = VM_INHERIT_DEFAULT;

	kern_return_t kr = mach_vm_map(target, &address, size_rounded, mask, flags,
		object, offset, copy, cur_protection, max_protection, inheritance);
	MALLOC_ASSERT(kr == KERN_SUCCESS);
	return address;
}

static void
quarantine_vm_deallocate(vm_address_t addr, size_t size)
{
	vm_map_t target = mach_task_self();
	mach_vm_address_t address = (mach_vm_address_t)addr;
	mach_vm_size_t size_rounded = round_page(size);
	kern_return_t kr = mach_vm_deallocate(target, address, size_rounded);
	MALLOC_ASSERT(kr == KERN_SUCCESS);
}

static void
quarantine_vm_protect(vm_address_t addr, size_t size, vm_prot_t protection)
{
	vm_map_t target = mach_task_self();
	mach_vm_address_t address = (mach_vm_address_t)addr;
	mach_vm_size_t size_rounded = round_page(size);
	bool set_maximum = false;
	kern_return_t kr = mach_vm_protect(target, address, size_rounded, set_maximum, protection);
	MALLOC_ASSERT(kr == KERN_SUCCESS);
}

// Env helpers
static const char *
env_var(const char *name)
{
	const char **env = (const char **)*_NSGetEnviron();
	return _simple_getenv(env, name);
}

static bool
env_bool(const char *name)
{
	const char *value = env_var(name);
	if (!value) return false;
	return value[0] == '1';
}

static uint32_t
env_uint(const char *name, uint32_t default_value)
{
	const char *value = env_var(name);
	if (!value) return default_value;
	return (uint32_t)strtoul(value, NULL, 0);
}

// Shadow memory helpers
#define SHADOW_MEMORY_BASE (0x0000200000000000ull)
#define PTR_TO_SHADOW(p) (void *)((((uintptr_t)ptr) >> 3) + SHADOW_MEMORY_BASE)
#define ROUND_UP(n, multiple_of) (((n + multiple_of - 1) / multiple_of) * multiple_of)
#define SIZE_TO_SHADOW_SIZE(p) (ROUND_UP(p, 16) >> 3)

static void
poison(void *ptr, size_t size)
{
	memset(PTR_TO_SHADOW(ptr), 0xff, SIZE_TO_SHADOW_SIZE(size));
}

static void
unpoison(void *ptr, size_t size)
{
	memset(PTR_TO_SHADOW(ptr), 0x0, SIZE_TO_SHADOW_SIZE(size));
}

static uint32_t
stacktrace_depo_insert(struct stacktrace_depo_t *depo, vm_address_t *pcs, size_t count);

static bool
pointer_map_find(struct pointer_map_t *map, uintptr_t ptr, uint64_t *word_out);

static void
pointer_map_insert(struct pointer_map_t *map, uintptr_t ptr, uint64_t word);

#define countof(a) (sizeof(a) / sizeof(*(a)))
#define wrap(index, container) ((index) & (countof(container) - 1))

static uint32_t OS_ALWAYS_INLINE
insert_current_stacktrace_into_depo(struct stacktrace_depo_t *depo, uint32_t top_frames_to_ignore)
{
	vm_address_t pcs[16 + top_frames_to_ignore];
	uint32_t num_pcs;
	thread_stack_pcs(pcs, (unsigned)countof(pcs), &num_pcs);
	if (num_pcs <= top_frames_to_ignore) {
		return 0;
	}
	return stacktrace_depo_insert(depo, &pcs[top_frames_to_ignore], num_pcs - top_frames_to_ignore);
}

static void OS_ALWAYS_INLINE
record_alloc_stacktrace(struct stacktrace_depo_t *depo, struct pointer_map_t *map, void *ptr, size_t size)
{
	if (ptr == NULL || size >= PAGE_SIZE) {
		return;
	}
	uint32_t alloc_hash = insert_current_stacktrace_into_depo(depo, 1);
	pointer_map_insert(map, (uintptr_t)ptr, alloc_hash);
}


#pragma mark -
#pragma mark Quarantine Logic

typedef struct quarantined_chunk {
	uint64_t next_and_size;
	uint64_t stacktrace_hashes;
} quarantined_chunk_t;

MALLOC_STATIC_ASSERT(sizeof(quarantined_chunk_t) == 16,
		"quarantined_chunk_t must be 16 bytes to fit in all allocations");

typedef union {
	uint64_t i;
	struct {
		uint64_t next_ptr : 48;
		uint64_t size : 16;
	} parts;
} next_and_size;

MALLOC_STATIC_ASSERT(sizeof(next_and_size) == 8,
		"next_and_size must be 8 bytes");

static void OS_NOINLINE
place_into_quarantine(quarantine_zone_t *zone, void *ptr, size_t size)
{
	if (ptr == NULL) {
		return;
	}

	// We need to know the size of the chunk, for quarantine bookkeeping
	if (size == 0) {
		size = DELEGATE(size, ptr);
	}

	// Don't quarantine large allocations to avoid one single huge allocation
	// evicting the whole quarantine.
	if (size > PAGE_SIZE) {
		return DELEGATE(free, ptr);
	}

	if (zone->do_poisoning) {
		poison(ptr, size);
	}

	uint32_t dealloc_stack_hash = insert_current_stacktrace_into_depo(zone->depo, 2);
	uint64_t stored_word = 0;
	pointer_map_find(zone->map, (uintptr_t)ptr, &stored_word);
	uint32_t alloc_stack_hash = (uint32_t)stored_word;
	uint64_t hashes = alloc_stack_hash | (((uint64_t)dealloc_stack_hash) << 32);

	lock(zone);

	// Append ptr to the tail of the quarantine list
	if (zone->items_in_quarantine == 0) {
		zone->quarantine_tail = zone->quarantine_head = ptr;
	} else {
		next_and_size n;
		n.i = _malloc_read_uint64_via_rsp(&zone->quarantine_tail->next_and_size);
		n.parts.next_ptr = (uintptr_t)ptr;
		_malloc_write_uint64_via_rsp(&zone->quarantine_tail->next_and_size, n.i);
		zone->quarantine_tail = ptr;
	}
	next_and_size n = { .parts = { .next_ptr = 0, .size = size } };
	_malloc_write_uint64_via_rsp(&zone->quarantine_tail->next_and_size, n.i);
	_malloc_write_uint64_via_rsp(&zone->quarantine_tail->stacktrace_hashes, hashes);

	zone->items_in_quarantine += 1;
	zone->bytes_in_quarantine += size;

	// Now let's remove and free chunks from the quarantine list that are over
	// limits. To minimize the work that we do under the zone lock, we only
	// remove chunks from the quarantine list (i.e. we adjust quarantine_head
	// and statistics), and then only actually free the chunks outside of the
	// lock.
	long items_over_limit = (zone->max_items_in_quarantine > 0 &&
		zone->items_in_quarantine > zone->max_items_in_quarantine) ?
		zone->items_in_quarantine - zone->max_items_in_quarantine : 0;
	long bytes_over_limit = (zone->max_bytes_in_quarantine > 0 &&
		zone->bytes_in_quarantine > zone->max_bytes_in_quarantine) ?
		zone->bytes_in_quarantine - zone->max_bytes_in_quarantine : 0;

	quarantined_chunk_t *items_to_free_head = zone->quarantine_head;
	size_t items_to_free_count = 0;
	size_t items_to_free_size = 0;

	quarantined_chunk_t *iterator = zone->quarantine_head;
	while (items_over_limit > 0 || bytes_over_limit > 0) {
		next_and_size n;
		n.i = _malloc_read_uint64_via_rsp(&iterator->next_and_size);
		quarantined_chunk_t *next = (void *)n.parts.next_ptr;
		size_t iterator_size = n.parts.size;

		items_to_free_count += 1;
		items_to_free_size += iterator_size;
		items_over_limit -= 1;
		bytes_over_limit -= iterator_size;

		iterator = next;
	}

	zone->quarantine_head = iterator;
	zone->items_in_quarantine -= items_to_free_count;
	zone->bytes_in_quarantine -= items_to_free_size;

	unlock(zone);

	// Actually free chunks. At this point, they are already removed from the
	// quarantine list so we are the exclusive owner of them.
	iterator = items_to_free_head;
	for (size_t i = 0; i < items_to_free_count; i++) {
		next_and_size n;
		n.i = _malloc_read_uint64_via_rsp(&iterator->next_and_size);
		quarantined_chunk_t *next = (void *)n.parts.next_ptr;
		size_t iterator_size = n.parts.size;

		if (zone->do_poisoning) {
			unpoison(iterator, iterator_size);
		}
		if (zone->debug) malloc_report(ASL_LEVEL_INFO, "evicting %p from quarantine, size = 0x%lx\n", iterator, iterator_size);
		DELEGATE(free_definite_size, iterator, iterator_size);

		iterator = next;
	}
}

#pragma mark -
#pragma mark MurmurHash2

// 32-bit MurmurHash2, public domain by Austin Appleby,
// <https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp>.

#define MURMUR2_SEED 0xe3be96d1  // fair dice roll
#define MURMUR2_MULTIPLIER 0x5bd1e995

static uint32_t
murmur2_init()
{
	return MURMUR2_SEED;
}

static void
murmur2_add_uint32(uint32_t *hstate, uint32_t val)
{
	val *= MURMUR2_MULTIPLIER;
	val ^= val >> 24;
	val *= MURMUR2_MULTIPLIER;
	*hstate *= MURMUR2_MULTIPLIER;
	*hstate ^= val;
}

static void
murmur2_add_uintptr(uint32_t *hstate, uintptr_t ptr)
{
	murmur2_add_uint32(hstate, (uint32_t)ptr);
	murmur2_add_uint32(hstate, (uint32_t)(ptr >> 32));
}

static uint32_t
murmur2_finalize(uint32_t *hstate)
{
	uint32_t X = *hstate;
	X ^= X >> 13;
	X *= MURMUR2_MULTIPLIER;
	X ^= X >> 15;
	return X;
}

static uint32_t
murmur2_hash_pointer(uintptr_t ptr)
{
	uint32_t hstate = murmur2_init();
	murmur2_add_uintptr(&hstate, ptr);
	return murmur2_finalize(&hstate);
}

static uint32_t
murmur2_hash_backtrace(uintptr_t *pcs, size_t count)
{
	uint32_t hstate = murmur2_init();
	for (int i = 0; i < count; i++) {
		murmur2_add_uintptr(&hstate, pcs[i]);
	}
	return murmur2_finalize(&hstate);
}


#pragma mark -
#pragma mark Stack Trace Depo

// Data structure to store up to 512k unique stacktraces, if they're on average
// 8 frames large, barring hash collisions, loosely modelled after Scudo:
// <https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/scudo/>.
//
// - The "handle" to a stored stacktrace is its own hash (Murmur2).
// - Frames are stored in a ring buffer, oldest get replaced on wrap.
// - Look-up will not return evicted data because we check the hash.
// - Friendly for remote inspection by ReportCrash (no pointers).
// - Lock-free, non-synchronizing insertion (no in-process look-ups).
// - Racy same-hash insertion might store the frames twice, but that's fine.
// - Hash collisions (murmur2 produces uint32_t hashes) will cause a stacktrace
//   to be unique'd against a different one, and look-up can retrieve a wrong
//   stacktrace. Should be rare enough with a good hashing algorithm, and it's
//   fine given we store stacktraces only for diagnostic purposes.
//
// The quarantine zone captures alloc and dealloc stack traces and saves them
// into the depo. The handles/hashes are then stored elsewhere (in pointer_map
// for live allocations, and in quarantine_chunk_t for quarantined ones).

typedef struct stacktrace_depo_t {
	uint64_t index[1 << 19];  // 512k entries, 4 MiB in size
	uint64_t storage[1 << 22];  // 4m entries, 32 MiB in size
	uint64_t storage_pos; // can be over countof(storage), always use wrap()
} stacktrace_depo_t;

typedef union {
	uint64_t i;
	struct {
		uint32_t hash;
		uint32_t pos : 24;
		uint32_t count : 8;
	} parts;
} index_entry;
MALLOC_STATIC_ASSERT(sizeof(index_entry) == 8, "index_entry should be 64 bits");

static stacktrace_depo_t *
stacktrace_depo_create()
{
	return mvm_allocate_pages(sizeof(stacktrace_depo_t), PAGE_SIZE, 0, VM_MEMORY_ANALYSIS_TOOL);
}

static void
stacktrace_depo_destroy(stacktrace_depo_t *depo)
{
	mvm_deallocate_pages(depo, sizeof(stacktrace_depo_t), 0);
}

static uint32_t
stacktrace_depo_insert(stacktrace_depo_t *depo, vm_address_t *pcs, size_t count)
{
	MALLOC_ASSERT(count < 256);
	uint32_t hash = murmur2_hash_backtrace(pcs, count);
	uint32_t index_pos = wrap(hash, depo->index);

	index_entry entry;
	entry.i = os_atomic_load(&depo->index[index_pos], relaxed);
	if (entry.parts.count == count && entry.parts.hash == hash) {
		return hash;
	}

	uint64_t old_storage_pos = wrap(os_atomic_add_orig(&depo->storage_pos,
			count, relaxed), depo->storage);
	entry.parts.hash = hash;
	entry.parts.pos = (uint32_t)old_storage_pos;
	entry.parts.count = (uint32_t)count;
	os_atomic_store(&depo->index[index_pos], entry.i, relaxed);
	for (int i = 0; i < count; i++) {
		uint32_t pos = wrap(old_storage_pos + i, depo->storage);
		os_atomic_store(&depo->storage[pos], pcs[i], relaxed);
	}
	return hash;
}

// Doesn't need to use atomics or be thread-safe against insertion because
// look-up is only used from ReportCrash against a corpse.
static size_t
stacktrace_depo_find(stacktrace_depo_t *depo, uint32_t hash, vm_address_t *pcs, size_t max_size)
{
	uint32_t index_pos = wrap(hash, depo->index);

	index_entry entry;
	entry.i = depo->index[index_pos];
	if (entry.parts.hash != hash || entry.parts.pos > countof(depo->storage)) {
		return 0;
	}

	uint32_t hstate = murmur2_init();
	for (int i = 0; i < entry.parts.count; i++) {
		uint32_t pos = wrap(entry.parts.pos + i, depo->storage);
		if (i < max_size) {
			pcs[i] = depo->storage[pos];
		}
		murmur2_add_uintptr(&hstate, pcs[i]);
	}

	if (hash != murmur2_finalize(&hstate)) {
		return 0;
	}

	return MIN(max_size, entry.parts.count);
}


#pragma mark -
#pragma mark Pointer Map

// Data structure to associate and store a 64-bit value for arbitrary pointers.
//
// We use the pointer map to store handles/hashes of stacktraces for live heap
// allocations. When the same pointer is inserted again, it must have already
// been quarantined and free'd and recycled, so it's okay to drop the previous
// data associated with it. On slot collision (20 bits), we evict the older
// entry, in which case we just lose track of the associated allocation
// stacktrace for the older allocation. When a chunk is quarantined, we transfer
// the stacktrace handle into quarantine_chunk_t, so we no longer care about the
// pointer map holding the right value for it. Look-up will never return a wrong
// value, because it checks the pointer address in the storage.

typedef struct pointer_map_t {
	__uint128_t storage[1 << 20];  // 1m entries, 16 MiB in size
} pointer_map_t;

typedef union {
	__uint128_t i;
	struct {
		uint64_t ptr;
		uint64_t word;
	} parts;
} pointer_map_entry;

MALLOC_STATIC_ASSERT(sizeof(pointer_map_entry) == 16, "pointer_map_entry should be 16 bytes");

static pointer_map_t *
pointer_map_create()
{
	return mvm_allocate_pages(sizeof(pointer_map_t), PAGE_SIZE, 0, VM_MEMORY_ANALYSIS_TOOL);
}

static void
pointer_map_destroy(pointer_map_t *map)
{
	mvm_deallocate_pages(map, sizeof(pointer_map_t), 0);
}

static void
pointer_map_insert(pointer_map_t *map, uintptr_t ptr, uint64_t word)
{
	uint32_t hash = murmur2_hash_pointer(ptr);
	uint32_t pos = wrap(hash, map->storage);
	pointer_map_entry entry;
	entry.parts.ptr = ptr;
	entry.parts.word = word;
	os_atomic_store_wide(&map->storage[pos], entry.i, relaxed);
}

static bool
pointer_map_find(pointer_map_t *map, uintptr_t ptr, uint64_t *word_out)
{
	uint32_t hash = murmur2_hash_pointer(ptr);
	uint32_t pos = wrap(hash, map->storage);
	pointer_map_entry entry;
	entry.i = os_atomic_load_wide(&map->storage[pos], relaxed);
	if (entry.parts.ptr != ptr) {
		return false;
	}
	*word_out = entry.parts.word;
	return true;
}


#pragma mark -
#pragma mark Zone Functions

static size_t
quarantine_size(quarantine_zone_t *zone, const void *ptr)
{
	return DELEGATE(size, ptr);
}

static void *
quarantine_malloc(quarantine_zone_t *zone, size_t size)
{
	void *ptr = DELEGATE(malloc, size);
	record_alloc_stacktrace(zone->depo, zone->map, ptr, size);
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "malloc(0x%lx) = %p\n", size, ptr);
	return ptr;
}

static void *
quarantine_calloc(quarantine_zone_t *zone, size_t num_items, size_t size)
{
	void *ptr = DELEGATE(calloc, num_items, size);
	record_alloc_stacktrace(zone->depo, zone->map, ptr, num_items * size);
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "calloc(0x%lx, 0x%lx) = %p\n", num_items, size, ptr);
	return ptr;
}

static void *
quarantine_valloc(quarantine_zone_t *zone, size_t size)
{
	void *ptr = DELEGATE(valloc, size);
	record_alloc_stacktrace(zone->depo, zone->map, ptr, size);
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "valloc(0x%lx) = %p\n", size, ptr);
	return ptr;
}

static void
quarantine_free(quarantine_zone_t *zone, void *ptr)
{
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "free(%p)\n", ptr);
	place_into_quarantine(zone, ptr, 0);
}

static void *
quarantine_realloc(quarantine_zone_t *zone, void *ptr, size_t new_size)
{
	if (ptr == NULL) {
		void *new_ptr = DELEGATE(malloc, new_size);
		record_alloc_stacktrace(zone->depo, zone->map, new_ptr, new_size);
		if (zone->debug) malloc_report(ASL_LEVEL_INFO, "realloc(NULL, 0x%lx) = %p\n", new_size, new_ptr);
		return new_ptr;
	}

	if (new_size == 0) {
		new_size = 1;
	}

	size_t old_size = DELEGATE(size, ptr);
	void *new_ptr = DELEGATE(malloc, new_size);
	record_alloc_stacktrace(zone->depo, zone->map, new_ptr, new_size);
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "realloc(%p, 0x%lx) = %p (old_size = 0x%lx)\n", ptr, new_size, new_ptr, old_size);

	// Don't free/quarantine the old pointer if allocation failed. Per man page:
	// > For realloc(), the input pointer is still valid if reallocation failed.
	if (new_ptr == NULL) {
		return NULL;
	}

	memcpy(new_ptr, ptr, MIN(old_size, new_size));
	place_into_quarantine(zone, ptr, old_size);
	return new_ptr;
}

static void
quarantine_destroy(quarantine_zone_t *zone)
{
	stacktrace_depo_destroy(zone->depo);
	pointer_map_destroy(zone->map);
	malloc_destroy_zone(zone->wrapped_zone);
	quarantine_vm_deallocate((vm_address_t)zone, sizeof(quarantine_zone_t));
}

static void *
quarantine_memalign(quarantine_zone_t *zone, size_t alignment, size_t size)
{
	void *ptr = DELEGATE(memalign, alignment, size);
	record_alloc_stacktrace(zone->depo, zone->map, ptr, size);
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "memalign(0x%lx, 0x%lx)\n", alignment, size);
	return ptr;
}

static void
quarantine_free_definite_size(quarantine_zone_t *zone, void *ptr, size_t size)
{
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "free_definite_size(%p, 0x%lx)\n", ptr, size);
	place_into_quarantine(zone, ptr, size);
}

static unsigned
quarantine_batch_malloc(quarantine_zone_t *zone, size_t size, void **results, unsigned count)
{
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "batch_malloc(0x%lx, %p, 0x%x)\n", size, results, count);
	return 0;
}

static void
quarantine_batch_free(quarantine_zone_t *zone, void **to_be_freed, unsigned count)
{
	if (zone->debug) malloc_report(ASL_LEVEL_INFO, "batch_free(%p, 0x%x)\n", to_be_freed, count);
	for (long i = 0; i < count; i++) {
		place_into_quarantine(zone, to_be_freed[i], 0);
	}
}

static size_t
quarantine_pressure_relief(quarantine_zone_t *zone, size_t goal)
{
	return DELEGATE(pressure_relief, goal);
}

static bool
quarantine_claimed_address(quarantine_zone_t *zone, void *ptr)
{
	return DELEGATE(claimed_address, ptr);
}

#pragma mark -
#pragma mark Introspection Functions

static kern_return_t
quarantine_enumerator(task_t task, void *context, unsigned type_mask, vm_address_t zone_address, memory_reader_t reader, vm_range_recorder_t recorder)
{
	return KERN_NOT_SUPPORTED;
}

static void
quarantine_statistics(quarantine_zone_t *zone, malloc_statistics_t *stats)
{
}

static kern_return_t
quarantine_statistics_task(task_t task, vm_address_t zone_address, memory_reader_t reader, malloc_statistics_t *stats)
{
	return KERN_NOT_SUPPORTED;
}

static void
quarantine_print(quarantine_zone_t *zone, bool verbose)
{
}

static void
quarantine_print_task(task_t task, unsigned level, vm_address_t zone_address, memory_reader_t reader, print_task_printer_t printer)
{
}

static void
quarantine_log(quarantine_zone_t *zone, void *address)
{
}

static size_t
quarantine_good_size(quarantine_zone_t *zone, size_t size)
{
	return DELEGATE(introspect->good_size, size);
}

static bool
quarantine_check(quarantine_zone_t *zone)
{
	return true; // Zone is always in a consistent state.
}

static void
quarantine_force_lock(quarantine_zone_t *zone)
{
	lock(zone);
}

static void
quarantine_force_unlock(quarantine_zone_t *zone)
{
	unlock(zone);
}

static void
quarantine_reinit_lock(quarantine_zone_t *zone)
{
	init_lock(zone);
}

static bool
quarantine_zone_locked(quarantine_zone_t *zone)
{
	bool lock_taken = trylock(zone);
	if (lock_taken) {
		unlock(zone);
	}
	return !lock_taken;
}


#pragma mark -
#pragma mark Crash Reporter API

static _malloc_lock_s crash_reporter_lock = _MALLOC_LOCK_INIT;

static crash_reporter_memory_reader_t g_crm_reader;
static const uint32_t k_max_read_memory = 1024;
static void *read_memory[k_max_read_memory];
static uint32_t num_read_memory;

static kern_return_t
memory_reader_adapter(task_t task, vm_address_t address, vm_size_t size, void **local_memory)
{
	MALLOC_ASSERT(num_read_memory < k_max_read_memory);
	void *ptr = g_crm_reader(task, address, size);
	*local_memory = ptr;
	read_memory[num_read_memory++] = ptr;
	return ptr ? KERN_SUCCESS : KERN_FAILURE;
}

static struct {
	vm_address_t address_to_lookup;
	vm_range_t found_range;
} enumeration_context;

static void
pointer_recorder(task_t task, void *context, unsigned type, vm_range_t *ranges, unsigned count)
{
	vm_address_t a = enumeration_context.address_to_lookup;
	for (int i = 0; i < count; i++) {
		if (ranges[i].address <= a && a < ranges[i].address + ranges[i].size) {
			enumeration_context.found_range = ranges[i];
			break;
		}
	}
}

kern_return_t
quarantine_diagnose_fault_from_crash_reporter(vm_address_t fault_address, quarantine_report_t *report,
		task_t task, vm_address_t zone_address, crash_reporter_memory_reader_t crm_reader)
{
	_malloc_lock_lock(&crash_reporter_lock);

	#define COPY_FROM_REMOTE(p, type) crm_reader(task, (vm_address_t)p, sizeof(type))
	quarantine_zone_t *remote_zone = COPY_FROM_REMOTE(zone_address, quarantine_zone_t);
	pointer_map_t *remote_pointer_map = COPY_FROM_REMOTE(remote_zone->map, pointer_map_t);
	stacktrace_depo_t *remote_depo = COPY_FROM_REMOTE(remote_zone->depo, stacktrace_depo_t);

	enumeration_context.found_range.address = 0;
	enumeration_context.found_range.size = 0;
	enumeration_context.address_to_lookup = fault_address;

	g_crm_reader = crm_reader;
	num_read_memory = 0;

	// We rely on being able to perform zone enumeration across different architecture slices on macOS.
	// On Apple Silicon Macs, ReportCrash is always running as a native (arm64e) process, but we also
	// need to be able to inspect x86_64 targets that are running under Rosetta. So the data layout and
	// zone logic needs to match between x86_64 and arm64(e).
	szone_introspect.enumerator(task, NULL, MALLOC_PTR_IN_USE_RANGE_TYPE, (vm_address_t)remote_zone->wrapped_zone,
								memory_reader_adapter, pointer_recorder);
	for (uint32_t i = 0; i < num_read_memory; i++) {
		free(read_memory[i]);
	}
	g_crm_reader = NULL;

	memset(report, 0, sizeof(*report));
	report->fault_address = fault_address;

	if (enumeration_context.found_range.address != 0) {
		report->nearest_allocation = enumeration_context.found_range.address;
		report->allocation_size = enumeration_context.found_range.size;

		quarantined_chunk_t *chunk = COPY_FROM_REMOTE(enumeration_context.found_range.address, quarantined_chunk_t);
		uint32_t alloc_handle = (uint32_t)chunk->stacktrace_hashes;
		uint32_t dealloc_handle = (uint32_t)(chunk->stacktrace_hashes >> 32);

		report->alloc_trace.thread_id = 0;
		report->alloc_trace.num_frames = (uint32_t)stacktrace_depo_find(remote_depo, alloc_handle,
				report->alloc_trace.frames, countof(report->alloc_trace.frames));

		report->dealloc_trace.thread_id = 0;
		report->dealloc_trace.num_frames = (uint32_t)stacktrace_depo_find(remote_depo, dealloc_handle,
				report->dealloc_trace.frames, countof(report->dealloc_trace.frames));

		free(chunk);
	}

	free(remote_depo);
	free(remote_pointer_map);
	free(remote_zone);

	_malloc_lock_unlock(&crash_reporter_lock);
	return KERN_SUCCESS;
}


#pragma mark -
#pragma mark Zone Templates

// Suppress warning: incompatible function pointer types
#define FN_PTR(fn) (void *)(&fn)

static malloc_introspection_t quarantine_zone_introspect_template = {
	// Block and region enumeration
	.enumerator = FN_PTR(quarantine_enumerator),

	// Statistics
	.statistics = FN_PTR(quarantine_statistics),
	.task_statistics = FN_PTR(quarantine_statistics_task),

	// Logging
	.print = FN_PTR(quarantine_print),
	.print_task = FN_PTR(quarantine_print_task),
	.log = FN_PTR(quarantine_log),

	// Queries
	.good_size = FN_PTR(quarantine_good_size),
	.check = FN_PTR(quarantine_check),

	// Locking
	.force_lock = FN_PTR(quarantine_force_lock),
	.force_unlock = FN_PTR(quarantine_force_unlock),
	.reinit_lock = FN_PTR(quarantine_reinit_lock),
	.zone_locked = FN_PTR(quarantine_zone_locked),

	// Discharge checking
	.enable_discharge_checking = NULL,
	.disable_discharge_checking = NULL,
	.discharge = NULL,
#ifdef __BLOCKS__
	.enumerate_discharged_pointers = NULL,
#else
	.enumerate_unavailable_without_blocks = NULL,
#endif
};

static const malloc_zone_t malloc_zone_template = {
	// Reserved for CFAllocator
	.reserved1 = NULL,
	.reserved2 = NULL,

	// Standard operations
	.size = FN_PTR(quarantine_size),
	.malloc = FN_PTR(quarantine_malloc),
	.calloc = FN_PTR(quarantine_calloc),
	.valloc = FN_PTR(quarantine_valloc),
	.free = FN_PTR(quarantine_free),
	.realloc = FN_PTR(quarantine_realloc),
	.destroy = FN_PTR(quarantine_destroy),

	// Batch operations
	.batch_malloc = FN_PTR(quarantine_batch_malloc),
	.batch_free = FN_PTR(quarantine_batch_free),

	// Introspection
	.zone_name = "QuarantineMallocZone",
	.version = 12,
	.introspect = &quarantine_zone_introspect_template,

	// Specialized operations
	.memalign = FN_PTR(quarantine_memalign),
	.free_definite_size = FN_PTR(quarantine_free_definite_size),
	.pressure_relief = FN_PTR(quarantine_pressure_relief),
	.claimed_address = FN_PTR(quarantine_claimed_address)
};


#pragma mark -
#pragma mark Zone Configuration & Creation

bool
quarantine_should_enable(void)
{
	return env_bool("MallocQuarantineZone");
}

void
quarantine_reset_environment(void)
{
	// Unset MallocQuarantineZone from the environment to avoid propagating it
	// to any child processes (posix_spawn, exec, fork).
	unsetenv("MallocQuarantineZone");
}

malloc_zone_t *
quarantine_create_zone(malloc_zone_t *wrapped_zone)
{
	quarantine_zone_t *zone = (quarantine_zone_t *)quarantine_vm_map(sizeof(quarantine_zone_t),
		VM_PROT_READ | VM_PROT_WRITE, VM_MEMORY_MALLOC);
	zone->malloc_zone = malloc_zone_template;

	// Since we are calling szone_introspect.enumerator directly, see
	// quarantine_diagnose_fault_from_crash_reporter.
	MALLOC_ASSERT(wrapped_zone->introspect == &szone_introspect);
	zone->wrapped_zone = wrapped_zone;

	zone->debug = env_bool("MallocQuarantineZoneDebug");
	zone->do_poisoning = !env_bool("MallocQuarantineNoPoisoning");
	zone->max_items_in_quarantine = env_uint("MallocQuarantineMaxItems", 0); // default is 0 = unlimited
	zone->max_bytes_in_quarantine = (size_t)env_uint("MallocQuarantineMaxSizeInMB", 256) << 20; // 256 MB is default

	zone->depo = stacktrace_depo_create();
	zone->map = pointer_map_create();

	// Init mutable state
	init_lock(zone);
	quarantine_vm_protect((vm_address_t)zone, PAGE_MAX_SIZE, VM_PROT_READ);
	return (malloc_zone_t *)zone;
}

#else // CONFIG_QUARANTINE

kern_return_t
quarantine_diagnose_fault_from_crash_reporter(vm_address_t fault_address, quarantine_report_t *report,
		task_t task, vm_address_t zone_address, crash_reporter_memory_reader_t crm_reader)
{
	return KERN_NOT_SUPPORTED;
}

#endif // CONFIG_QUARANTINE
