/*
 * Copyright (c) 2000-2003, 2008, 2012 Apple Inc. All rights reserved.
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
/*
 * Copyright 1996 1995 by Open Software Foundation, Inc. 1997 1996 1995 1994 1993 1992 1991
 *              All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appears in all copies and
 * that both the copyright notice and this permission notice appear in
 * supporting documentation.
 *
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT,
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
/*
 * MkLinux
 */

#include "internal.h"

#if !defined(__OPEN_SOURCE__) && TARGET_OS_OSX // 40703288
/*
 * Machine specific support for thread initialization
 */

// NOTE: no resolvers, so this file must not contain any atomic operations

PTHREAD_NOEXPORT void _pthread_setup_suspended(pthread_t th, void (*f)(pthread_t), void *sp);

/*
 * Set up the initial state of a MACH thread
 */
void
_pthread_setup_suspended(pthread_t thread,
	       void (*routine)(pthread_t),
	       void *vsp)
{
#if defined(__i386__)
	i386_thread_state_t state = { };
	thread_state_flavor_t flavor = x86_THREAD_STATE32;
	mach_msg_type_number_t count = i386_THREAD_STATE_COUNT;
#elif defined(__x86_64__)
	x86_thread_state64_t state = { };
	thread_state_flavor_t flavor = x86_THREAD_STATE64;
	mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
#else
#error _pthread_setup not defined for this architecture
#endif

	(void)thread_get_state(_pthread_kernel_thread(thread),
			flavor, (thread_state_t)&state, &count);

#if defined(__i386__)
	uintptr_t *sp = vsp;

	state.__eip = (uintptr_t)routine;

	// We need to simulate a 16-byte aligned stack frame as if we had
	// executed a call instruction. Since we're "pushing" one argument,
	// we need to adjust the pointer by 12 bytes (3 * sizeof (int *))
	sp -= 3;			// make sure stack is aligned
	*--sp = (uintptr_t)thread;	// argument to function
	*--sp = 0;			// fake return address
	state.__esp = (uintptr_t)sp;	// set stack pointer
#elif defined(__x86_64__)
	uintptr_t *sp = vsp;

	state.__rip = (uintptr_t)routine;

	// We need to simulate a 16-byte aligned stack frame as if we had
	// executed a call instruction. The stack should already be aligned
	// before it comes to us and we don't need to push any arguments,
	// so we shouldn't need to change it.
	state.__rdi = (uintptr_t)thread;	// argument to function
	*--sp = 0;				// fake return address
	state.__rsp = (uintptr_t)sp;		// set stack pointer
#else
#error _pthread_setup_suspended not defined for this architecture
#endif

	(void)thread_set_state(_pthread_kernel_thread(thread), flavor, (thread_state_t)&state, count);
}
#endif // !defined(__OPEN_SOURCE__) && TARGET_OS_OSX
