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
#ifndef __MALLOC_COMMON_H
#define __MALLOC_COMMON_H

MALLOC_NOEXPORT
const char *
malloc_common_strstr(const char *src, const char *target, size_t target_len);

MALLOC_NOEXPORT
long
malloc_common_convert_to_long(const char *ptr, const char **end_ptr);

MALLOC_NOEXPORT
const char *
malloc_common_value_for_key(const char *src, const char *key);

MALLOC_NOEXPORT
const char *
malloc_common_value_for_key_copy(const char *src, const char *key,
		 char *bufp, size_t maxlen);

__options_decl(malloc_zone_options_t, unsigned, {
	MZ_NONE  = 0x0,
	MZ_POSIX = 0x1,
	MZ_C11   = 0x2,
});

static MALLOC_INLINE void
malloc_set_errno_fast(malloc_zone_options_t mzo, int err)
{
	if (mzo & MZ_POSIX) {
#if TARGET_OS_SIMULATOR
		errno = err;
#else
		(*_pthread_errno_address_direct()) = err;
#endif
	}
}

MALLOC_NOEXPORT
void
find_zone_and_free(void *ptr, bool known_non_default);

#endif // __MALLOC_COMMON_H
