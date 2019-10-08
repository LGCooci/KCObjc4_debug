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
    auto_zone.cpp
    Automatic Garbage Collection
    Copyright (c) 2002-2011 Apple Inc. All rights reserved.
 */

#define AUTO_USE_NEW_WEAK_CALLBACK

#include "auto_zone.h"
#include <stdlib.h>

auto_zone_t *auto_zone_create(const char *name) {
    abort();
}


struct malloc_introspection_t auto_zone_introspection() {
    abort();
}


void auto_zone_retain(auto_zone_t *zone, void *ptr) {
    abort();
}


unsigned int auto_zone_release(auto_zone_t *zone, void *ptr) {
    abort();
}


unsigned int auto_zone_retain_count(auto_zone_t *zone, const void *ptr) {
    abort();
}


const void *auto_zone_base_pointer(auto_zone_t *zone, const void *ptr) {
    abort();
}


boolean_t auto_zone_is_valid_pointer(auto_zone_t *zone, const void *ptr) {
    abort();
}


size_t auto_zone_size(auto_zone_t *zone, const void *ptr) {
    abort();
}


boolean_t auto_zone_set_write_barrier(auto_zone_t *zone, const void *dest, const void *new_value) {
    abort();
}


boolean_t auto_zone_atomicCompareAndSwap(auto_zone_t *zone, void *existingValue, void *newValue, void *volatile *location, boolean_t isGlobal, boolean_t issueBarrier) {
    abort();
}


boolean_t auto_zone_atomicCompareAndSwapPtr(auto_zone_t *zone, void *existingValue, void *newValue, void *volatile *location, boolean_t issueBarrier) {
    abort();
}


void *auto_zone_write_barrier_memmove(auto_zone_t *zone, void *dst, const void *src, size_t size) {
    abort();
}


void *auto_zone_strong_read_barrier(auto_zone_t *zone, void **source) {
    abort();
}


void auto_zone_statistics(auto_zone_t *zone, auto_statistics_t *stats) {
    abort();
}


auto_collection_control_t *auto_collection_parameters(auto_zone_t *zone) {
    abort();
}


void auto_collector_disable(auto_zone_t *zone) {
    abort();
}


void auto_collector_reenable(auto_zone_t *zone) {
    abort();
}


boolean_t auto_zone_is_enabled(auto_zone_t *zone) {
    abort();
}


boolean_t auto_zone_is_collecting(auto_zone_t *zone) {
    abort();
}


void auto_collect(auto_zone_t *zone, auto_collection_mode_t mode, void *collection_context) {
    abort();
}


void auto_collect_multithreaded(auto_zone_t *zone) {
    abort();
}


void auto_zone_collect(auto_zone_t *zone, auto_zone_options_t options) {
    abort();
}


void auto_zone_collect_and_notify(auto_zone_t *zone, auto_zone_options_t options, dispatch_queue_t callback_queue, dispatch_block_t completion_callback) {
    abort();
}


void auto_zone_compact(auto_zone_t *zone, auto_zone_compact_options_t options, dispatch_queue_t callback_queue, dispatch_block_t completion_callback) {
    abort();
}


void auto_zone_disable_compaction(auto_zone_t *zone) {
    abort();
}


void auto_zone_register_resource_tracker(auto_zone_t *zone, const char *description, boolean_t (^should_collect)(void)) {
    abort();
}


void auto_zone_unregister_resource_tracker(auto_zone_t *zone, const char *description) {
    abort();
}


void auto_zone_reap_all_local_blocks(auto_zone_t *zone) {
    abort();
}


auto_memory_type_t auto_zone_get_layout_type(auto_zone_t *zone, void *ptr) {
    abort();
}


void* auto_zone_allocate_object(auto_zone_t *zone, size_t size, auto_memory_type_t type, boolean_t initial_refcount_to_one, boolean_t clear) {
    abort();
}


unsigned auto_zone_batch_allocate(auto_zone_t *zone, size_t size, auto_memory_type_t type, boolean_t initial_refcount_to_one, boolean_t clear, void **results, unsigned num_requested) {
    abort();
}


void *auto_zone_create_copy(auto_zone_t *zone, void *ptr) {
    abort();
}


void auto_zone_register_thread(auto_zone_t *zone) {
    abort();
}


void auto_zone_unregister_thread(auto_zone_t *zone) {
    abort();
}


void auto_zone_assert_thread_registered(auto_zone_t *zone) {
    abort();
}


void auto_zone_register_datasegment(auto_zone_t *zone, void *address, size_t size) {
    abort();
}


void auto_zone_unregister_datasegment(auto_zone_t *zone, void *address, size_t size) {
    abort();
}


void auto_assign_weak_reference(auto_zone_t *zone, const void *value, const void **location, auto_weak_callback_block_t *block) {
    abort();
}


void* auto_read_weak_reference(auto_zone_t *zone, void **referrer) {
    abort();
}


void auto_zone_set_compaction_observer(auto_zone_t *zone, void *block, void (^observer) (void)) {
    abort();
}


void auto_zone_add_root(auto_zone_t *zone, void *address_of_root_ptr, void *value) {
    abort();
}


void auto_zone_remove_root(auto_zone_t *zone, void *address_of_root_ptr) {
    abort();
}


void auto_zone_root_write_barrier(auto_zone_t *zone, void *address_of_possible_root_ptr, void *value) {
    abort();
}


void auto_zone_set_associative_ref(auto_zone_t *zone, void *object, void *key, void *value) {
    abort();
}


void *auto_zone_get_associative_ref(auto_zone_t *zone, void *object,  void *key) {
    abort();
}


void auto_zone_erase_associative_refs(auto_zone_t *zone, void *object) {
    abort();
}


size_t auto_zone_get_associative_hash(auto_zone_t *zone, void *object) {
    abort();
}


void auto_zone_enumerate_associative_refs(auto_zone_t *zone, void *key, boolean_t (^block) (void *object, void *value)) {
    abort();
}


boolean_t auto_zone_enable_collection_checking(auto_zone_t *zone) {
    abort();
}


void auto_zone_disable_collection_checking(auto_zone_t *zone) {
    abort();
}


void auto_zone_track_pointer(auto_zone_t *zone, void *pointer) {
    abort();
}


void auto_zone_enumerate_uncollected(auto_zone_t *zone, auto_zone_collection_checking_callback_t callback) {
    abort();
}


boolean_t auto_zone_is_finalized(auto_zone_t *zone, const void *ptr) {
    abort();
}


void auto_zone_set_nofinalize(auto_zone_t *zone, void *ptr) {
    abort();
}


void auto_zone_set_unscanned(auto_zone_t *zone, void *ptr) {
    abort();
}


void auto_zone_set_scan_exactly(auto_zone_t *zone, void *ptr) {
    abort();
}


void auto_zone_clear_stack(auto_zone_t *zone, unsigned long options) {
    abort();
}


void (*__auto_reference_logger)(uint32_t eventtype, void *ptr, uintptr_t data);


void auto_enumerate_references(auto_zone_t *zone, void *referent, 
                               auto_reference_recorder_t callback, 
                               void *stack_bottom, void *ctx) {
    abort();
}


void **auto_weak_find_first_referrer(auto_zone_t *zone, void **location, unsigned long count) {
    abort();
}


auto_zone_t *auto_zone(void) {
    return 0;
}


void auto_zone_dump(auto_zone_t *zone,
                    auto_zone_stack_dump stack_dump,
                    auto_zone_register_dump register_dump,
                    auto_zone_node_dump thread_local_node_dump, // unsupported
                    auto_zone_root_dump root_dump,
                    auto_zone_node_dump global_node_dump,
                    auto_zone_weak_dump weak_dump) {
    abort();
}


void auto_zone_visit(auto_zone_t *zone, auto_zone_visitor_t *visitor) {
    abort();
}


auto_probe_results_t auto_zone_probe_unlocked(auto_zone_t *zone, void *address) {
    abort();
}


void auto_zone_scan_exact(auto_zone_t *zone, void *address, void (^callback)(void *base, unsigned long byte_offset, void *candidate)) {
    abort();
}


