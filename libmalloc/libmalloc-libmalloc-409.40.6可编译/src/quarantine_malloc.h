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

#ifndef _QUARANTINE_MALLOC_H_
#define _QUARANTINE_MALLOC_H_

#include "base.h"
#include "malloc/malloc.h"
#include <stdbool.h>

MALLOC_NOEXPORT
bool
quarantine_should_enable(void);

MALLOC_NOEXPORT
void
quarantine_reset_environment(void);

MALLOC_NOEXPORT
malloc_zone_t *
quarantine_create_zone(malloc_zone_t *wrapped_zone);

static inline uint16_t
_malloc_read_uint16_via_rsp(void *ptr)
{
#if TARGET_CPU_X86_64
    __asm__ (
        "subq  %%rsp,       %0  \n"
        "movw (%%rsp, %0),  %w0 \n"
        : "+r" (ptr)            // outputs, ptr = %0 read-write
        :                       // inputs, empty
        :                       // clobbers, empty
    );
    return (uint16_t)(uintptr_t)ptr;
#elif TARGET_CPU_ARM64
	__asm__ (
		"sub  %0, %0, fp    \n"
		"ldrh %w0, [fp, %0] \n"
		: "+r" (ptr)            // outputs, ptr = %0 read-write
		:                       // inputs, empty
		:                       // clobbers, empty
	);
	return (uint16_t)(uintptr_t)ptr;
#else
    return *(uint16_t *)ptr;
#endif
}

static inline uint64_t
_malloc_read_uint64_via_rsp(void *ptr)
{
#if TARGET_CPU_X86_64
    __asm__ (
        "subq  %%rsp,       %0  \n"
        "movq (%%rsp, %0),  %0  \n"
        : "+r" (ptr)            // outputs, ptr = %0 read-write
        :                       // inputs, empty
        :                       // clobbers, empty
    );
    return (uint64_t)ptr;
#elif TARGET_CPU_ARM64
	__asm__ (
		"sub %0, %0, fp  \n"
		"ldr %0, [fp, %0]\n"
		: "+r" (ptr)            // outputs, ptr = %0 read-write
		:                       // inputs, empty
		:                       // clobbers, empty
	);
	return (uint64_t)ptr;
#else
    return *(uint64_t *)ptr;
#endif
}

static inline void
_malloc_write_uint64_via_rsp(void *ptr, uint64_t value)
{
#if TARGET_CPU_X86_64
    __asm__ (
        "subq  %%rsp,  %0         \n"
        "movq  %1,    (%%rsp, %0) \n"
        :                         // outputs, empty
        : "r" (ptr), "r" (value)  // inputs, ptr = %0, value = %1
        :                         // clobbers, empty
    );
#elif TARGET_CPU_ARM64
	__asm__ volatile (
		"sub %0, %0, fp   \n"
		"str %1, [fp, %0] \n"
		: "+r" (ptr)              // outputs, ptr = %0 (not a real output but gets clobbered)
		: "r" (value)             // inputs, value = %1
		:                         // clobbers, empty
	);
#else
    *(uint64_t *)ptr = value;
#endif
}

#endif // _QUARANTINE_MALLOC_H_
