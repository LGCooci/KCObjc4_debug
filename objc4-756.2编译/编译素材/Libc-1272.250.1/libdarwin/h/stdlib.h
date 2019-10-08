/*
 * Copyright (c) 2018 Apple Inc. All rights reserved.
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

/*!
 * @header
 * Non-standard, Darwin-specific additions to the stdlib(3) family of APIs.
 *
 * The os_malloc() and os_strdup() routines are wrappers to be used for small,
 * fixed-size allocations, the assumption being that such allocation should
 * always succeed absent other critical problems. Thus, if the requested size is
 * is a compile-time constant, the return value is asserted to be non-NULL.
 * Otherwise, for sizes that are not known at compile-time, the implementations
 * loop until the allocation succeeds, assuming the failure to be due to
 * transient resource shortages. The implementation will not loop if the program
 * has not become multi-threaded, under the assertion that there would be no
 * point since no other thread could possibly free up resources for the calling
 * thread to use. Thus, in a single-threaded program, all allocations will
 * be treated like small, fixed-size allocations and be expected to succeed.
 *
 * These wrappers should not be used for large allocations whose bounds cannot
 * be determined at compile-time. For such allocations, malloc(3), calloc(3), et
 * al. (with appropriate error handling) are the appropriate interfaces.
 */
#ifndef __DARWIN_STDLIB_H
#define __DARWIN_STDLIB_H

#include <os/base.h>
#include <os/api.h>
#include <os/assumes.h>
#include <dispatch/private.h>

#include <stdlib.h>
#include <sys/types.h>
#include <sys/cdefs.h>

__BEGIN_DECLS;

/*!
 * @function __os_temporary_resource_shortage
 * A function whose purpose is to appear in stack traces to indicate transient
 * resource shortage conditions. Do not call.
 */
DARWIN_API_AVAILABLE_20170407
OS_EXPORT OS_NOT_TAIL_CALLED
void
__os_temporary_resource_shortage(void);

/*!
 * @functiongroup
 * Internal inline definitions.
 */
DARWIN_API_AVAILABLE_20170407
OS_ALWAYS_INLINE OS_WARN_RESULT OS_MALLOC __alloc_size(1)
static inline void *
_os_malloc_loop(size_t size)
{
	void *ptr = NULL;
	while (!(ptr = malloc(size))) {
		__os_temporary_resource_shortage();
	}
	return ptr;
}

DARWIN_API_AVAILABLE_20170407
OS_ALWAYS_INLINE OS_WARN_RESULT OS_MALLOC __alloc_size(1)
static inline void *
_os_malloc_known(size_t size)
{
	return malloc(size);
}

DARWIN_API_AVAILABLE_20170407
OS_ALWAYS_INLINE OS_WARN_RESULT OS_MALLOC __alloc_size(1, 2)
static inline void *
_os_calloc_loop(size_t cnt, size_t size)
{
	void *ptr = NULL;
	while (!(ptr = calloc(cnt, size))) {
		__os_temporary_resource_shortage();
	}
	return ptr;
}

DARWIN_API_AVAILABLE_20170407
OS_ALWAYS_INLINE OS_WARN_RESULT OS_MALLOC __alloc_size(1, 2)
static inline void *
_os_calloc_known(size_t cnt, size_t size)
{
	return calloc(cnt, size);
}

DARWIN_API_AVAILABLE_20170407
OS_ALWAYS_INLINE OS_WARN_RESULT OS_MALLOC
static inline char *
_os_strdup_loop(const char *str)
{
	char *ptr = NULL;
	while (!(ptr = strdup(str))) {
		__os_temporary_resource_shortage();
	}
	return ptr;
}

DARWIN_API_AVAILABLE_20170407
OS_ALWAYS_INLINE OS_WARN_RESULT OS_MALLOC
static inline char *
_os_strdup_known(const char *str)
{
	return strdup(str);
}

/*!
 * @function os_malloc
 * Wrapper around malloc(3) which guarantees that the allocation succeeds.
 *
 * @param __size
 * The size of the allocation.
 *
 * @result
 * A new object that the caller is responsible for free(3)ing.
 *
 * This routine will never return NULL. If the size of the allocation is known
 * at compile-time, a failure to allocate the object will abort the caller. If
 * the size is not known at compile-time, the routine will retry until it is
 * successful.
 */
#define os_malloc(__size) ({ \
	void *ptr = NULL; \
	size_t _size = (__size); \
	if (__builtin_constant_p(__size) || !_dispatch_is_multithreaded()) { \
		ptr = _os_malloc_known(_size); \
		os_assert_malloc("known-constant allocation", ptr, _size); \
	} else { \
		ptr = _os_malloc_loop(_size); \
	} \
	(ptr); \
})

/*!
 * @function os_calloc
 * Wrapper around calloc(3) which guarantees that the allocation succeeds.
 *
 * @param __cnt
 * The number of elements to allocate.
 *
 * @param __size
 * The size of each element to allocate.
 *
 * @result
 * A new object that the caller is responsible for free(3)ing.
 *
 * This routine will never return NULL. If the size of the allocation is known
 * at compile-time, a failure to allocate the object will abort the caller. If
 * the size is not known at compile-time, the routine will retry until it is
 * successful.
 */
#define os_calloc(__cnt, __size) ({ \
	void *ptr = NULL; \
	size_t _size = (__size); \
	size_t _cnt = (__size); \
	if ((__builtin_constant_p(__cnt) && __builtin_constant_p(__size)) || \
			 !_dispatch_is_multithreaded()) { \
		ptr = _os_calloc_known(_cnt, _size); \
		os_assert_malloc("known-constant allocation", ptr, _size); \
	} else { \
		ptr = _os_calloc_loop(_cnt, _size); \
	} \
	(ptr); \
})

/*!
 * @function os_strdup
 * A wrapper around strdup(3) which guarantees that the string duplication
 * succeeds.
 *
 * @param __str
 * The string to duplicate.
 *
 * @result
 * A new string that the caller is responsible for free(3)ing.
 *
 * This routine will never return NULL. If the given string is a compile-time
 * constant, a failure to duplicate it will abort the caller. If the string is
 * not a compile-time constant, the routine will retry until it is successful.
 *
 * @discussion
 * strdup(3) is found in the string(3) API family, but this interface is in the
 * stdlib.h header because its semantic changes are solely related to the manner
 * in which memory is allocated.
 */
#define os_strdup(__str) ({ \
	char *ptr = NULL; \
	const char *_str = (__str); \
	if (__builtin_constant_p(__str) || !_dispatch_is_multithreaded()) { \
		ptr = _os_strdup_known(_str); \
		os_assert_malloc("known-constant allocation", ptr, strlen(_str)); \
	} else { \
		ptr = _os_strdup_loop(_str); \
	} \
	(ptr); \
})

/*!
 * @function os_localtime_file
 * A routine to generate a time stamp that is suitable for embedding in a file
 * name.
 *
 * @param buff
 * A pointer to a buffer where the resulting time stamp will be stored.
 *
 * @discussion
 * The resulting time stamp will not include characters which require escaping
 * in shells, such as spaces. The current implementation format is
 *
 * YYYY-MM-DD_HH.MM.SS.us
 *
 * e.g.
 *
 * 2017-04-24_12.45.15.045609
 */
DARWIN_API_AVAILABLE_20170407
OS_EXPORT
void
os_localtime_file(char buff[32]);

/*!
 * @function os_simple_hash
 * An implementation of a simple non-cryptographic hashing algorithm.
 *
 * @param buff
 * A pointer to the buffer to hash.
 *
 * @param len
 * The length of the buffer.
 *
 * @result
 * The hashed value of the input.
 *
 * @discussion
 * This routine is meant to be used as a simple way to obtain a value that can
 * be used to choose a bucket in a simple hash table. Do not attach security
 * assumptions to the output of this routine. Do not assume thst the computed
 * hash is stable between hosts, OS versions, or boot sessions.
 */
DARWIN_API_AVAILABLE_20170407
OS_EXPORT OS_NONNULL1
uint64_t
os_simple_hash(const void *buff, size_t len);

/*!
 * @function os_simple_hash_string
 * An implementation of a simple non-cryptographic hashing algorithm.
 *
 * @param string
 * A pointer to the null-terminated string to hash.
 *
 * @result
 * The hashed value of the input.
 *
 * @discussion
 * This routine is the moral equivalent of a call to
 *
 * os_simple_hash(buff, strlen(buff));
 *
 * All the same considerations of {@link os_simple_hash} apply.
 */
DARWIN_API_AVAILABLE_20170407
OS_EXPORT OS_NONNULL1
uint64_t
os_simple_hash_string(const char *string);

__END_DECLS;

#endif // __DARWIN_STDLIB_H
