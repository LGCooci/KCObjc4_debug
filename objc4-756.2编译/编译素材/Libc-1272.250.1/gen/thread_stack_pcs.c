/*
 * Copyright (c) 1999-2018 Apple Inc. All rights reserved.
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

/*	Bertrand from vmutils -> CF -> System */

#include <pthread.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <stdlib.h>
#include "stack_logging.h"


#if defined(__i386__) || defined(__x86_64__) || defined(__arm__) || defined(__arm64__)
#define FP_LINK_OFFSET 1
#else
#error ********** Unimplemented architecture
#endif


#define	INSTACK(a)	((a) >= stackbot && (a) <= stacktop)
#if defined(__x86_64__)
#define	ISALIGNED(a)	((((uintptr_t)(a)) & 0xf) == 0)
#elif defined(__i386__)
#define	ISALIGNED(a)	((((uintptr_t)(a)) & 0xf) == 8)
#elif defined(__arm__) || defined(__arm64__)
#define	ISALIGNED(a)	((((uintptr_t)(a)) & 0x1) == 0)
#endif

__private_extern__  __attribute__((noinline))
void
_thread_stack_pcs(vm_address_t *buffer, unsigned max, unsigned *nb,
		unsigned skip, void *startfp)
{
	void *frame, *next;
	pthread_t self = pthread_self();
	void *stacktop = pthread_get_stackaddr_np(self);
	void *stackbot = stacktop - pthread_get_stacksize_np(self);

	*nb = 0;

	/* make sure return address is never out of bounds */
	stacktop -= (FP_LINK_OFFSET + 1) * sizeof(void *);

	frame = __builtin_frame_address(0);
	if(!INSTACK(frame) || !ISALIGNED(frame))
		return;
	while ((startfp && startfp >= *(void **)frame) || skip--) {
		next = *(void **)frame;
		if(!INSTACK(next) || !ISALIGNED(next) || next <= frame)
			return;
		frame = next;
	}
	while (max--) {
		void *retaddr = (void *)*(vm_address_t *)
				(((void **)frame) + FP_LINK_OFFSET);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wint-conversion"
		buffer[*nb] = retaddr;
#pragma clang diagnostic pop
		(*nb)++;
		next = *(void **)frame;
		if(!INSTACK(next) || !ISALIGNED(next) || next <= frame)
			return;
		frame = next;
	}
}

// Prevent thread_stack_pcs() from getting tail-call-optimized into
// _thread_stack_pcs() on 64-bit environments, thus making the "number of hot
// frames to skip" be more predictable, giving more consistent backtraces.
//
// See <rdar://problem/5364825> "stack logging: frames keep getting truncated"
// for why this is necessary.
__attribute__((disable_tail_calls))
void
thread_stack_pcs(vm_address_t *buffer, unsigned max, unsigned *nb)
{
	_thread_stack_pcs(buffer, max, nb, 0, NULL);
}
