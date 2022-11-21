/*
 * Copyright (c) 1999-2016 Apple Inc. All rights reserved.
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

#ifndef _MALLOC_PRIVATE_H_
#define _MALLOC_PRIVATE_H_

/* Here be dragons (SPIs) */

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>
#include <Availability.h>
#include <os/availability.h>
#include <malloc/malloc.h>

/*********	Callbacks	************/

API_AVAILABLE(macos(10.12), ios(10.0), tvos(10.0), watchos(3.0))
void malloc_enter_process_memory_limit_warn_mode(void);
	/* A callback invoked once the process receives a warning for approaching
	 * memory limit. */

__OSX_AVAILABLE(10.12) __IOS_AVAILABLE(10.0)
__TVOS_AVAILABLE(10.0) __WATCHOS_AVAILABLE(3.0)
void malloc_memory_event_handler(unsigned long);
	/* A function invoked when malloc needs to handle any flavor of
	 * memory pressure notification or process memory limit notification. */

API_AVAILABLE(macos(10.12), ios(10.0), tvos(10.0), watchos(3.0))
void * reallocarray(void * in_ptr, size_t nmemb, size_t size) __DARWIN_EXTSN(reallocarray) __result_use_check;

API_AVAILABLE(macos(10.12), ios(10.0), tvos(10.0), watchos(3.0))
void * reallocarrayf(void * in_ptr, size_t nmemb, size_t size) __DARWIN_EXTSN(reallocarrayf) __result_use_check;

/*
 * Checks whether an address might belong to any registered zone. False positives
 * are allowed (e.g. the memory was freed, or it's in a part of the address
 * space used by malloc that has not yet been allocated.) False negatives are
 * not allowed.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
boolean_t malloc_claimed_address(void *ptr) __result_use_check;

/*
 * Checks whether an address might belong to a given zone. False positives are
 * allowed (e.g. the memory was freed, or it's in a part of the address space
 * used by malloc that has not yet been allocated.) False negatives are not
 * allowed.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
boolean_t malloc_zone_claimed_address(malloc_zone_t *zone, void *ptr) __result_use_check;

/**
 * Returns whether the nano allocator is engaged. The return value is 0 if Nano
 * is not engaged and the allocator version otherwise.
 */
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
int malloc_engaged_nano(void) __result_use_check;

/*
 * Disables zero-on-free in a process.  This has security implications and is
 * intended to be used only as part of binary compatibility workarounds for
 * external code.  It should be called as early as possible in the process
 * lifetime, ideally before the process has gone multithreaded.  It is not
 * guaranteed to have any effect.
 */
SPI_AVAILABLE(macos(13.0), ios(16.1), tvos(16.1), watchos(9.1))
void malloc_zero_on_free_disable(void);

/****** Thread-specific libmalloc options ******/

/**
 * Options struct: zero means "default options".
 */
typedef struct {
	uintptr_t DisableExpensiveDebuggingOptions : 1;
	uintptr_t DisableProbabilisticGuardMalloc : 1;
	uintptr_t DisableMallocStackLogging : 1;
} malloc_thread_options_t;

API_AVAILABLE(macos(13.0), ios(16.0), tvos(16.0), watchos(9.0))
malloc_thread_options_t malloc_get_thread_options(void) __result_use_check;

API_AVAILABLE(macos(13.0), ios(16.0), tvos(16.0), watchos(9.0))
void malloc_set_thread_options(malloc_thread_options_t opts);

/****** Crash Reporter integration ******/

typedef struct {
	uint64_t thread_id;
	uint64_t time;
	uint32_t num_frames;
	vm_address_t frames[64];
} stack_trace_t;

/**
 * Like memory_reader_t, but caller must free returned memory if not NULL.
 */
typedef void *(*crash_reporter_memory_reader_t)(task_t task, vm_address_t address, size_t size);

/****** Probabilistic Guard Malloc ******/

/**
 * Never sample any allocations made by the current thread.
 *
 * DEPRECATED!  Use malloc_set_thread_options() instead.  Will be removed soon.
 */
void pgm_disable_for_current_thread(void);

typedef struct {
	// diagnose_page_fault
	const char *error_type;
	const char *confidence;
	vm_address_t fault_address;
	// fill_in_report
	vm_address_t nearest_allocation;
	size_t allocation_size;
	const char *allocation_state;
	uint32_t num_traces;
	// fill_in_trace
	stack_trace_t alloc_trace;
	stack_trace_t dealloc_trace;
} pgm_report_t;

kern_return_t pgm_diagnose_fault_from_crash_reporter(vm_address_t fault_address, pgm_report_t *report,
		task_t task, vm_address_t zone_address, crash_reporter_memory_reader_t crm_reader) __result_use_check;

/****** Quarantine Zone ******/

typedef struct {
	vm_address_t fault_address;
	vm_address_t nearest_allocation;
	size_t allocation_size;
	stack_trace_t alloc_trace;
	stack_trace_t dealloc_trace;
} quarantine_report_t;

kern_return_t quarantine_diagnose_fault_from_crash_reporter(vm_address_t fault_address, quarantine_report_t *report,
		task_t task, vm_address_t zone_address, crash_reporter_memory_reader_t crm_reader) __result_use_check;


#endif /* _MALLOC_PRIVATE_H_ */
