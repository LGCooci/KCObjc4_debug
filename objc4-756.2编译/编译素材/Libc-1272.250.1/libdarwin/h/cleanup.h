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
 * Attributes to handle automatic clean-up of certain types of variables when
 * they go out of scope.
 *
 * IMPORTANT: These attributes will NOT cause a variable to be cleaned up when
 * its value changes. For example, this pattern would leak:
 *
 * void *__os_free ptr = malloc(10);
 * ptr = somewhere_else;
 * return;
 *
 * You should only use these attributes for very well-scoped, temporary
 * allocations.
 */
#ifndef __DARWIN_CLEANUP_H
#define __DARWIN_CLEANUP_H

#include <os/base.h>
#include <os/api.h>
#include <os/assumes.h>
#include <os/object_private.h>

#include <sys/errno.h>
#include <sys/cdefs.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <mach/mach_init.h>
#include <mach/port.h>
#include <mach/mach_port.h>
#include <mach/kern_return.h>

__BEGIN_DECLS;

#if __has_attribute(cleanup)
/*!
 * @define __os_free
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to free(3) when it goes out of scope. Applying this
 * attribute to variables that do not reference heap allocations will result in
 * undefined behavior.
 */
#define __os_free __attribute__((cleanup(__os_cleanup_free)))
static inline void
__os_cleanup_free(void *__p)
{
	void **tp = (void **)__p;
	void *p = *tp;
	free(p);
}

/*!
 * @define __os_close
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to close(2) when it goes out of scope. Applying
 * this attribute to variables that do not reference a valid file descriptor
 * will result in undefined behavior. If the variable's value is -1 upon going
 * out-of-scope, no cleanup is performed.
 */
#define __os_close __attribute__((cleanup(__os_cleanup_close)))
static inline void
__os_cleanup_close(int *__fd)
{
	int fd = *__fd;
	if (fd == -1) {
		return;
	}
	posix_assert_zero(close(fd));
}

/*!
 * @define __os_fclose
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to fclose(3) when it goes out of scope. Applying
 * this attribute to variables that do not reference a valid FILE* will result
 * in undefined behavior. If the variable's value is NULL upon going out-of-
 * scope, no cleanup is performed.
 */
#define __os_fclose __attribute__((cleanup(__os_cleanup_fclose)))
static inline void
__os_cleanup_fclose(FILE **__fp)
{
	FILE *f = *__fp;
	int ret = -1;

	if (!f) {
		return;
	}

	ret = fclose(f);
	if (ret == EOF) {
		os_assert_zero(errno);
	}
}

/*!
 * @define __os_close_mach_recv
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to {@link darwin_mach_port_close_recv} when it goes
 * out of scope. Applying this attribute to variables that do not reference a
 * valid Mach port receive right will result in undefined behavior. If the
 * variable's value is MACH_PORT_NULL or MACH_PORT_DEAD upon going out-of-scope,
 * no cleanup is performed.
 */
#define __os_close_mach_recv \
		__attribute__((cleanup(__os_cleanup_close_mach_recv)))
static inline void
__os_cleanup_close_mach_recv(mach_port_t *__p)
{
	mach_port_t p = *__p;
	kern_return_t kr = KERN_FAILURE;

	if (!MACH_PORT_VALID(p)) {
		return;
	}

	kr = mach_port_destruct(mach_task_self(), p, 0, 0);
	os_assert_zero(kr);
}

/*!
 * @define __os_release_mach_send
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to {@link darwin_mach_port_release} when it goes
 * out of scope. Applying this attribute to variables that do not reference a
 * valid Mach port send right or MACH_PORT_NULL or MACH_PORT_DEAD will result
 * in undefined behavior. If the variable's value is MACH_PORT_NULL or
 * MACH_PORT_DEAD upon going out-of-scope, no cleanup is performed.
 */
#define __os_release_mach_send \
		__attribute__((cleanup(__os_cleanup_release_mach_send)))
static inline void
__os_cleanup_release_mach_send(mach_port_t *__p)
{
	mach_port_t p = *__p;
	kern_return_t kr = KERN_FAILURE;

	if (!MACH_PORT_VALID(p)) {
		return;
	}

	kr = mach_port_deallocate(mach_task_self(), p);
	os_assert_zero(kr);
}

/*!
 * @define __os_preserve_errno
 * An attribute that may be applied to a variable's type. This attribute sets
 * the global errno to the value of the variable when the variable goes out of
 * scope. This attribute is useful for preserving the value of errno upon entry
 * to a function and guaranteeing that it is restored upon exit.
 */
#define __os_preserve_errno \
		__unused __attribute__((cleanup(__os_cleanup_errno)))
static inline void
__os_cleanup_errno(int *__e)
{
	errno = *__e;
}

/*!
 * @define __os_release
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to os_release() when it goes out of scope. Applying
 * this attribute to a variable which does not reference a valid os_object_t
 * object will result in undefined behavior. If the variable's value is NULL
 * upon going out-of-scope, no cleanup is performed.
 *
 * This attribute may be applied to dispatch and XPC objects.
 */
#define __os_release __attribute__((cleanup(__os_cleanup_os_release)))
static inline void
__os_cleanup_os_release(void *__p)
{
	_os_object_t *tp = (_os_object_t *)__p;
	_os_object_t o = *tp;
	if (!o) {
		return;
	}
	os_release(o);
}

#if __COREFOUNDATION__
/*!
 * @define __os_cfrelease
 * An attribute that may be applied to a variable's type. This attribute causes
 * the variable to be passed to CFRelease() when it goes out of scope. Applying
 * this attribute to a variable which does not reference a valid CoreFoundation
 * object will result in undefined behavior. If the variable's value is NULL
 * upon going out-of-scope, no cleanup is performed.
 */
#define __os_cfrelease __attribute__((cleanup(__os_cleanup_cfrelease)))
static inline void
__os_cleanup_cfrelease(void *__p)
{
	CFTypeRef *tp = (CFTypeRef *)__p;
	CFTypeRef cf = *tp;
	if (!cf) {
		return;
	}
	CFRelease(cf);
}
#endif // __COREFOUNDATION__

#else // __has_attribute(cleanup)
#define __os_free __attribute__((__os_not_supported))
#define __os_close __attribute__((__os_not_supported))
#define __os_fclose __attribute__((__os_not_supported))
#define __os_close_mach_recv __attribute__((__os_not_supported))
#define __os_release_mach_send __attribute__((__os_not_supported))
#define __os_preserve_errno __attribute__((__os_not_supported))
#define __os_release __attribute__((__os_not_supported))
#define __os_cfrelease __attribute__((__os_not_supported))
#endif // __has_attribute(cleanup)

__END_DECLS;

#endif // __DARWIN_CLEANUP_H
