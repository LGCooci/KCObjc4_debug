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
#include "internal.h"

#pragma mark Utilities
static int
_sysctl_12809455(int mib[4], size_t mib_cnt, void *old, size_t *old_len,
		void *new, size_t new_len)
{
	int error = -1;
	int ret = -1;
	size_t mylen = 0;
	size_t *mylenp = NULL;
#if RDAR_12809455
	bool workaround_12809455 = false;
#endif

	if (old_len) {
		mylen = *old_len;
		mylenp = &mylen;
	}

	// sysctl(3) doesn't behave anything like its documentation leads you to
	// believe. If the given buffer is too small to hold the requested data,
	// what's supposed to happen is:
	//
	//   - as much data as possible is copied into the buffer
	//   - the number of bytes copied is written to the len parameter
	//   - errno is set to ENOMEM
	//   - -1 is returned (to indicate that you should check errno)
	//
	// What actually happens:
	//
	//   - no data is copied
	//   - len is set to zero
	//   - errno is undefined
	//   - zero is returned
	//
	// So... swing and a miss.
	//
	// Since it returns success in this case our only indication that something
	// went wrong is if mylen is set to zero.
	//
	// So we do our best to sniff out the misbehavior and emulate sysctl(3)'s
	// API contract until it's fixed.
	//
	// <rdar://problem/12809455>
#if RDAR_12809455
	if (old_len && *old_len > 0) {
		// We can only work around the bug if the passed-in length was non-zero.
		workaround_12809455 = true;
	}
#endif

	ret = sysctl(mib, (u_int)mib_cnt, old, mylenp, new, new_len);
#if RDAR_12809455
	if (workaround_12809455 && old && ret == 0 && mylen == 0) {
		ret = -1;
		errno = ENOMEM;
	}
#endif // RDAR_12809455

	if (ret == 0) {
		error = 0;
	} else {
		error = errno;
	}

	if (old_len) {
		*old_len = mylen;
	}

	return error;
}

static char *
_strblk(const char *str)
{
	const char *cur = str;

	while (*cur && !isblank(*cur)) {
		cur++;
	}

	return (char *)cur;
}

static bool
_get_boot_arg_value(const char *which, char *where, size_t max)
{
	// This is (very) loosely based on the implementation of
	// PE_parse_boot_argn() (or at least the parts where I was able to easily
	// decipher the policy).
	bool found = false;
	errno_t error = -1;
	char *buff = NULL;
	size_t buff_len = 0;
	char *theone = NULL;
	char *equals = NULL;

	error = sysctlbyname_get_data_np("kern.bootargs", (void **)&buff,
			&buff_len);
	if (error) {
		goto __out;
	}

	theone = strstr(buff, which);
	if (!theone) {
		goto __out;
	}

	found = true;
	if (!where) {
		// Caller just wants to know whether the boot-arg exists.
		goto __out;
	}

	equals = strchr(theone, '=');
	if (!equals || isblank(equals[1])) {
		strlcpy(where, "", max);
	} else {
		char *nextsep = NULL;
		char nextsep_old = 0;

		// Find the next separator and nerf it temporarily for the benefit of
		// the underlying strcpy(3).
		nextsep = _strblk(theone);
		nextsep_old = *nextsep;
		*nextsep = 0;
		strlcpy(where, &equals[1], max);

		*nextsep = nextsep_old;
	}

__out:
	free(buff);
	return found;
}

#pragma mark API
errno_t
sysctl_get_data_np(int mib[4], size_t mib_cnt, void **buff, size_t *buff_len)
{
	errno_t error = -1;
	size_t needed = 0;
	void *mybuff = NULL;

	// We need to get the length of the parameter so we can allocate a buffer
	// that's large enough.
	error = _sysctl_12809455(mib, mib_cnt, NULL, &needed, NULL, 0);
	if (error) {
		goto __out;
	}

	mybuff = malloc(needed);
	if (!mybuff) {
		error = errno;
		goto __out;
	}

	error = _sysctl_12809455(mib, mib_cnt, mybuff, &needed, NULL, 0);
	if (error) {
		// It's conceivable that some other process came along within this
		// window and modified the variable to be even larger than we'd
		// previously been told, but if that's the case, just give up.
		goto __out;
	}

	*buff = mybuff;
	*buff_len = needed;

__out:
	if (error) {
		free(mybuff);
	}
	return error;
}

errno_t
sysctlbyname_get_data_np(const char *mibdesc, void **buff, size_t *buff_len)
{
	int ret = -1;
	int error = -1;
	int mib[4];
	size_t mib_cnt = countof(mib);

	ret = sysctlnametomib(mibdesc, mib, &mib_cnt);
	if (ret) {
		error = errno;
		goto __out;
	}

	error = sysctl_get_data_np(mib, mib_cnt, buff, buff_len);

__out:
	return error;
}

bool
os_parse_boot_arg_int(const char *which, int64_t *where)
{
	bool found = false;
	char buff[24] = {0};
	char *endptr = NULL;
	int64_t val = 0;

	found = _get_boot_arg_value(which, buff, sizeof(buff));
	if (!found || !where) {
		goto __out;
	}

	// A base of zero handles bases 8, 10, and 16.
	val = strtoll(buff, &endptr, 0);
	if (*endptr == 0) {
		*where = val;
	} else {
		// The boot-arg value was invalid, so say we didn't find it.
		found = false;
	}

__out:
	return found;
}

bool
os_parse_boot_arg_string(const char *which, char *where, size_t maxlen)
{
	return _get_boot_arg_value(which, where, maxlen);
}
