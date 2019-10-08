/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */
/*
    auto_zone.h
    Automatic Garbage Collection.
    Copyright (c) 2002-2011 Apple Inc. All rights reserved.
 */

#ifndef __AUTO_ZONE__
#define __AUTO_ZONE__

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <malloc/malloc.h>
#include <Availability.h>
#include <AvailabilityMacros.h>
#include <TargetConditionals.h>

#include <dispatch/dispatch.h>

#define AUTO_EXPORT extern __attribute__((visibility("default")))

__BEGIN_DECLS

typedef malloc_zone_t auto_zone_t;
AUTO_EXPORT auto_zone_t *auto_zone_create(const char *name);
AUTO_EXPORT struct malloc_introspection_t auto_zone_introspection();
#define AUTO_RETAINED_BLOCK_TYPE 0x100  /* zone enumerator returns only blocks with nonzero retain count */
AUTO_EXPORT void auto_zone_retain(auto_zone_t *zone, void *ptr);
AUTO_EXPORT unsigned int auto_zone_release(auto_zone_t *zone, void *ptr);
AUTO_EXPORT unsigned int auto_zone_retain_count(auto_zone_t *zone, const void *ptr);
AUTO_EXPORT const void *auto_zone_base_pointer(auto_zone_t *zone, const void *ptr);
AUTO_EXPORT boolean_t auto_zone_is_valid_pointer(auto_zone_t *zone, const void *ptr);
AUTO_EXPORT size_t auto_zone_size(auto_zone_t *zone, const void *ptr);
AUTO_EXPORT boolean_t auto_zone_set_write_barrier(auto_zone_t *zone, const void *dest, const void *new_value);
AUTO_EXPORT boolean_t auto_zone_atomicCompareAndSwap(auto_zone_t *zone, void *existingValue, void *newValue, void *volatile *location, boolean_t isGlobal, boolean_t issueBarrier);
AUTO_EXPORT boolean_t auto_zone_atomicCompareAndSwapPtr(auto_zone_t *zone, void *existingValue, void *newValue, void *volatile *location, boolean_t issueBarrier);
AUTO_EXPORT void *auto_zone_write_barrier_memmove(auto_zone_t *zone, void *dst, const void *src, size_t size);
AUTO_EXPORT void *auto_zone_strong_read_barrier(auto_zone_t *zone, void **source);
typedef uint64_t auto_date_t;
typedef struct {
    auto_date_t     total_duration;
    auto_date_t     scan_duration;
    auto_date_t     enlivening_duration;
    auto_date_t     finalize_duration;
    auto_date_t     reclaim_duration;
} auto_collection_durations_t;
typedef struct {
    malloc_statistics_t malloc_statistics;
    uint32_t            version;            // set to 1 before calling
    size_t              num_collections[2];
    boolean_t           last_collection_was_generational;
    size_t              bytes_in_use_after_last_collection[2];
    size_t              bytes_allocated_after_last_collection[2];
    size_t              bytes_freed_during_last_collection[2];
    auto_collection_durations_t total[2];   // running total of each field
    auto_collection_durations_t last[2];    // most recent result
    auto_collection_durations_t maximum[2]; // on a per item basis, the max.  Thus, total != scan + finalize ...
    size_t              thread_collections_total;
    size_t              thread_blocks_recovered_total;
    size_t              thread_bytes_recovered_total;
} auto_statistics_t;
AUTO_EXPORT void auto_zone_statistics(auto_zone_t *zone, auto_statistics_t *stats);
enum {
    AUTO_COLLECT_RATIO_COLLECTION        = (0 << 0), // run generational or full depending on applying AUTO_COLLECTION_RATIO
    AUTO_COLLECT_GENERATIONAL_COLLECTION = (1 << 0), // collect young objects. Internal only.
    AUTO_COLLECT_FULL_COLLECTION         = (2 << 0), // collect entire heap. Internal only.
    AUTO_COLLECT_EXHAUSTIVE_COLLECTION   = (3 << 0), // run full collections until object count stabilizes.
    AUTO_COLLECT_SYNCHRONOUS             = (1 << 2), // block caller until scanning is finished.
    AUTO_COLLECT_IF_NEEDED               = (1 << 3), // only collect if AUTO_COLLECTION_THRESHOLD exceeded.
};
typedef uint32_t auto_collection_mode_t;
enum {
    AUTO_LOG_COLLECTIONS = (1 << 1),        // log block statistics whenever a collection occurs
    AUTO_LOG_TIMINGS = (1 << 2),            // log timing data whenever a collection occurs
    AUTO_LOG_REGIONS = (1 << 4),            // log whenever a new region is allocated
    AUTO_LOG_UNUSUAL = (1 << 5),            // log unusual circumstances
    AUTO_LOG_WEAK = (1 << 6),               // log weak reference manipulation
    AUTO_LOG_ALL = (~0u),
    AUTO_LOG_NONE = 0
};
typedef uint32_t auto_log_mask_t;
enum {
    AUTO_HEAP_HOLES_SHRINKING       = 1,        // total size of holes is approaching zero
    AUTO_HEAP_HOLES_EXHAUSTED       = 2,        // all holes exhausted, will use hitherto unused memory in "subzone"
    AUTO_HEAP_SUBZONE_EXHAUSTED     = 3,        // will add subzone
    AUTO_HEAP_REGION_EXHAUSTED      = 4,        // no more subzones available, need to add region
    AUTO_HEAP_ARENA_EXHAUSTED       = 5,        // arena exhausted.  (64-bit only)
};
typedef uint32_t auto_heap_growth_info_t;
typedef struct auto_zone_cursor *auto_zone_cursor_t;
typedef void (*auto_zone_foreach_object_t) (auto_zone_cursor_t cursor, void (*op) (void *ptr, void *data), void* data);
typedef struct {
    uint32_t        version;                    // sizeof(auto_collection_control_t)
    void            (*batch_invalidate) (auto_zone_t *zone, auto_zone_foreach_object_t foreach, auto_zone_cursor_t cursor, size_t cursor_size);
    void            (*resurrect) (auto_zone_t *zone, void *ptr);
    const unsigned char* (*layout_for_address)(auto_zone_t *zone, void *ptr);
    const unsigned char* (*weak_layout_for_address)(auto_zone_t *zone, void *ptr);
    char*           (*name_for_address) (auto_zone_t *zone, vm_address_t base, vm_address_t offset);
    auto_log_mask_t log;
    boolean_t       disable_generational;
    boolean_t       malloc_stack_logging;
    void            (*scan_external_callout)(void *context, void (*scanner)(void *context, void *start, void *end)) DEPRECATED_ATTRIBUTE;
    void            (*will_grow)(auto_zone_t *zone, auto_heap_growth_info_t) DEPRECATED_ATTRIBUTE;
    size_t          collection_threshold;
    size_t          full_vs_gen_frequency;
    const char*     (*name_for_object) (auto_zone_t *zone, void *object);
} auto_collection_control_t;
AUTO_EXPORT auto_collection_control_t *auto_collection_parameters(auto_zone_t *zone);
AUTO_EXPORT void auto_collector_disable(auto_zone_t *zone);
AUTO_EXPORT void auto_collector_reenable(auto_zone_t *zone);
AUTO_EXPORT boolean_t auto_zone_is_enabled(auto_zone_t *zone);
AUTO_EXPORT boolean_t auto_zone_is_collecting(auto_zone_t *zone);
AUTO_EXPORT void auto_collect(auto_zone_t *zone, auto_collection_mode_t mode, void *collection_context);
AUTO_EXPORT void auto_collect_multithreaded(auto_zone_t *zone);
enum {
    AUTO_ZONE_COLLECT_NO_OPTIONS = 0,
    AUTO_ZONE_COLLECT_RATIO_COLLECTION          = 1, // requests either a generational or a full collection, based on memory use heuristics.
    AUTO_ZONE_COLLECT_GENERATIONAL_COLLECTION   = 2, // requests a generational heap collection.
    AUTO_ZONE_COLLECT_FULL_COLLECTION           = 3, // requests a full heap collection.
    AUTO_ZONE_COLLECT_EXHAUSTIVE_COLLECTION     = 4, // requests an exhaustive heap collection.
    AUTO_ZONE_COLLECT_GLOBAL_MODE_MAX           = AUTO_ZONE_COLLECT_EXHAUSTIVE_COLLECTION, // the highest numbered global mode
    AUTO_ZONE_COLLECT_GLOBAL_MODE_COUNT         = AUTO_ZONE_COLLECT_EXHAUSTIVE_COLLECTION+1, // the highest numbered global mode
    AUTO_ZONE_COLLECT_GLOBAL_COLLECTION_MODE_MASK       = 0xf,    
    AUTO_ZONE_COLLECT_LOCAL_COLLECTION           = (1<<8),  // perform a normal thread local collection
    AUTO_ZONE_COLLECT_COALESCE                   = (1<<15), // allows the request to be skipped if a collection is in progress
};
typedef intptr_t auto_zone_options_t;
AUTO_EXPORT void auto_zone_collect(auto_zone_t *zone, auto_zone_options_t options);
#ifdef __BLOCKS__
AUTO_EXPORT void auto_zone_collect_and_notify(auto_zone_t *zone, auto_zone_options_t options, dispatch_queue_t callback_queue, dispatch_block_t completion_callback);
#endif
enum {
    AUTO_ZONE_COMPACT_NO_OPTIONS = 0,
    AUTO_ZONE_COMPACT_IF_IDLE = 1,          /* primes compaction to start after delay, if no dispatch threads intervene. */
    AUTO_ZONE_COMPACT_ANALYZE = 2,          /* runs a compaction analysis to file specified by environment variable. */
};
typedef uintptr_t auto_zone_compact_options_t;
#ifdef __BLOCKS__
AUTO_EXPORT void auto_zone_compact(auto_zone_t *zone, auto_zone_compact_options_t options, dispatch_queue_t callback_queue, dispatch_block_t completion_callback);
#endif
AUTO_EXPORT void auto_zone_disable_compaction(auto_zone_t *zone);
#ifdef __BLOCKS__
AUTO_EXPORT void auto_zone_register_resource_tracker(auto_zone_t *zone, const char *description, boolean_t (^should_collect)(void));
#endif
AUTO_EXPORT void auto_zone_unregister_resource_tracker(auto_zone_t *zone, const char *description);
AUTO_EXPORT void auto_zone_reap_all_local_blocks(auto_zone_t *zone);
enum {
    AUTO_TYPE_UNKNOWN =     -1,                                             // this is an error value
    // memory type bits.
    AUTO_UNSCANNED =        (1 << 0),
    AUTO_OBJECT =           (1 << 1),
    AUTO_POINTERS_ONLY =    (1 << 2),
    // allowed combinations of flags.
    AUTO_MEMORY_SCANNED = !AUTO_UNSCANNED,                                  // conservatively scanned memory
    AUTO_MEMORY_UNSCANNED = AUTO_UNSCANNED,                                 // unscanned memory (bits)
    AUTO_MEMORY_ALL_POINTERS = AUTO_POINTERS_ONLY,                          // scanned array of pointers
    AUTO_MEMORY_ALL_WEAK_POINTERS = (AUTO_UNSCANNED | AUTO_POINTERS_ONLY),  // unscanned, weak array of pointers
    AUTO_OBJECT_SCANNED = AUTO_OBJECT,                                      // object memory, exactly scanned with layout maps, conservatively scanned remainder, will be finalized
    AUTO_OBJECT_UNSCANNED = AUTO_OBJECT | AUTO_UNSCANNED,                   // unscanned object memory, will be finalized
    AUTO_OBJECT_ALL_POINTERS = AUTO_OBJECT | AUTO_POINTERS_ONLY             // object memory, exactly scanned with layout maps, all-pointers scanned remainder, will be finalized
};
typedef int32_t auto_memory_type_t;
AUTO_EXPORT auto_memory_type_t auto_zone_get_layout_type(auto_zone_t *zone, void *ptr);
AUTO_EXPORT void* auto_zone_allocate_object(auto_zone_t *zone, size_t size, auto_memory_type_t type, boolean_t initial_refcount_to_one, boolean_t clear);
AUTO_EXPORT unsigned auto_zone_batch_allocate(auto_zone_t *zone, size_t size, auto_memory_type_t type, boolean_t initial_refcount_to_one, boolean_t clear, void **results, unsigned num_requested);
AUTO_EXPORT void *auto_zone_create_copy(auto_zone_t *zone, void *ptr);
AUTO_EXPORT void auto_zone_register_thread(auto_zone_t *zone);
AUTO_EXPORT void auto_zone_unregister_thread(auto_zone_t *zone);
AUTO_EXPORT void auto_zone_assert_thread_registered(auto_zone_t *zone);
AUTO_EXPORT void auto_zone_register_datasegment(auto_zone_t *zone, void *address, size_t size);
AUTO_EXPORT void auto_zone_unregister_datasegment(auto_zone_t *zone, void *address, size_t size);
#if defined(AUTO_USE_NEW_WEAK_CALLBACK)
typedef struct new_auto_weak_callback_block auto_weak_callback_block_t;
#else
typedef struct old_auto_weak_callback_block auto_weak_callback_block_t;
#endif
struct new_auto_weak_callback_block {
    void    *isa;                                           // provides layout for exact scanning.
    auto_weak_callback_block_t *next;                       // must be set to zero before first use.
    void   (*callback_function)(void *__strong target);
    void    *__strong target;
};
struct old_auto_weak_callback_block {
    auto_weak_callback_block_t *next;                       // must be set to zero before first use.
    void (*callback_function)(void *arg1, void *arg2);
    void *arg1;
    void *arg2;
} DEPRECATED_ATTRIBUTE;
AUTO_EXPORT void auto_assign_weak_reference(auto_zone_t *zone, const void *value, const void **location, auto_weak_callback_block_t *block);
AUTO_EXPORT void* auto_read_weak_reference(auto_zone_t *zone, void **referrer);
#ifdef __BLOCKS__
AUTO_EXPORT void auto_zone_set_compaction_observer(auto_zone_t *zone, void *block, void (^observer) (void));
#endif
AUTO_EXPORT void auto_zone_add_root(auto_zone_t *zone, void *address_of_root_ptr, void *value);
AUTO_EXPORT void auto_zone_remove_root(auto_zone_t *zone, void *address_of_root_ptr);
AUTO_EXPORT void auto_zone_root_write_barrier(auto_zone_t *zone, void *address_of_possible_root_ptr, void *value);
AUTO_EXPORT void auto_zone_set_associative_ref(auto_zone_t *zone, void *object, void *key, void *value);
AUTO_EXPORT void *auto_zone_get_associative_ref(auto_zone_t *zone, void *object,  void *key);
AUTO_EXPORT void auto_zone_erase_associative_refs(auto_zone_t *zone, void *object);
AUTO_EXPORT size_t auto_zone_get_associative_hash(auto_zone_t *zone, void *object);
#ifdef __BLOCKS__
AUTO_EXPORT void auto_zone_enumerate_associative_refs(auto_zone_t *zone, void *key, boolean_t (^block) (void *object, void *value));
#endif
AUTO_EXPORT boolean_t auto_zone_enable_collection_checking(auto_zone_t *zone);
AUTO_EXPORT void auto_zone_disable_collection_checking(auto_zone_t *zone);
AUTO_EXPORT void auto_zone_track_pointer(auto_zone_t *zone, void *pointer);
#ifdef __BLOCKS__
typedef struct {
    boolean_t is_object;
    size_t survived_count;
} auto_zone_collection_checking_info;
typedef void (^auto_zone_collection_checking_callback_t)(void *pointer, auto_zone_collection_checking_info *info);
AUTO_EXPORT void auto_zone_enumerate_uncollected(auto_zone_t *zone, auto_zone_collection_checking_callback_t callback);
#endif
AUTO_EXPORT boolean_t auto_zone_is_finalized(auto_zone_t *zone, const void *ptr);
AUTO_EXPORT void auto_zone_set_nofinalize(auto_zone_t *zone, void *ptr);
AUTO_EXPORT void auto_zone_set_unscanned(auto_zone_t *zone, void *ptr);
AUTO_EXPORT void auto_zone_set_scan_exactly(auto_zone_t *zone, void *ptr);
AUTO_EXPORT void auto_zone_clear_stack(auto_zone_t *zone, unsigned long options);
enum {
    AUTO_RETAIN_EVENT = 14,
    AUTO_RELEASE_EVENT = 15
};
AUTO_EXPORT void (*__auto_reference_logger)(uint32_t eventtype, void *ptr, uintptr_t data);
typedef struct 
{
    vm_address_t referent;
    vm_address_t referrer_base;
    intptr_t     referrer_offset;
} auto_reference_t;
typedef void (*auto_reference_recorder_t)(auto_zone_t *zone, void *ctx, 
                                          auto_reference_t reference);
AUTO_EXPORT void auto_enumerate_references(auto_zone_t *zone, void *referent, 
                                      auto_reference_recorder_t callback, 
                                      void *stack_bottom, void *ctx);
AUTO_EXPORT void **auto_weak_find_first_referrer(auto_zone_t *zone, void **location, unsigned long count);
AUTO_EXPORT auto_zone_t *auto_zone(void);
#ifdef __BLOCKS__
typedef void (^auto_zone_stack_dump)(const void *base, unsigned long byte_size);
typedef void (^auto_zone_register_dump)(const void *base, unsigned long byte_size);
typedef void (^auto_zone_node_dump)(const void *address, unsigned long size, unsigned int layout, unsigned long refcount);
typedef void (^auto_zone_root_dump)(const void **address);
typedef void (^auto_zone_weak_dump)(const void **address, const void *item);
AUTO_EXPORT void auto_zone_dump(auto_zone_t *zone,
            auto_zone_stack_dump stack_dump,
            auto_zone_register_dump register_dump,
            auto_zone_node_dump thread_local_node_dump, // unsupported
            auto_zone_root_dump root_dump,
            auto_zone_node_dump global_node_dump,
            auto_zone_weak_dump weak_dump);
typedef struct {
    void *begin;
    void *end;
} auto_address_range_t;
typedef struct {
    uint32_t version;                    // sizeof(auto_zone_visitor_t)
    void (^visit_thread)(pthread_t thread, auto_address_range_t stack_range, auto_address_range_t registers);
    void (^visit_node)(const void *address, size_t size, auto_memory_type_t type, uint32_t refcount, boolean_t is_thread_local);
    void (^visit_root)(const void **address);
    void (^visit_weak)(const void *value, void *const*location, auto_weak_callback_block_t *callback);
    void (^visit_association)(const void *object, const void *key, const void *value);
} auto_zone_visitor_t;
AUTO_EXPORT void auto_zone_visit(auto_zone_t *zone, auto_zone_visitor_t *visitor);
enum {
    auto_is_not_auto  =    0,
    auto_is_auto      =    (1 << 1),   // always on for a start of a node
    auto_is_local     =    (1 << 2),   // is/was node local
};
typedef int auto_probe_results_t;
AUTO_EXPORT auto_probe_results_t auto_zone_probe_unlocked(auto_zone_t *zone, void *address);
AUTO_EXPORT void auto_zone_scan_exact(auto_zone_t *zone, void *address, void (^callback)(void *base, unsigned long byte_offset, void *candidate));

#endif

__END_DECLS

#endif /* __AUTO_ZONE__ */
