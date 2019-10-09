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
 * Non-standard, Darwin-specific additions for the stdio(3) family of APIs.
 */
#ifndef __DARWIN_STDIO_H
#define __DARWIN_STDIO_H

#include <os/base.h>
#include <os/api.h>
#include <sys/cdefs.h>

__BEGIN_DECLS;

/*!
 * @function zsnprintf_np
 * snprintf(3) variant which returns the numnber of bytes written less the null
 * terminator.
 *
 * @param buff
 * The buffer in which to write the string.
 *
 * @param len
 * The length of the buffer.
 *
 * @param fmt
 * The printf(3)-like format string.
 *
 * @param ...
 * The arguments corresponding to the format string.
 *
 * @result
 * The number of bytes written into the buffer, less the null terminator. This
 * routine is useful for successive string printing that may be lossy, as it
 * will simply return zero when there is no space left in the destination
 * buffer, i.e. enables the following pattern:
 *
 * char *cur = buff;
 * size_t left = sizeof(buff);
 * for (i = 0; i < n_strings; i++) {
 *     size_t n_written = zsnprintf_np(buff, left, "%s", strings[i]);
 *     cur += n_written;
 *     left -= n_written;
 * }
 *
 * This loop will safely terminate without any special care since, as soon as
 * the buffer's space is exhausted, all further calls to zsnprintf_np() will
 * write nothing and return zero.
 */
DARWIN_API_AVAILABLE_20170407
OS_EXPORT OS_WARN_RESULT OS_NONNULL1 OS_NONNULL3 OS_FORMAT_PRINTF(3, 4)
size_t
zsnprintf_np(char *buff, size_t len, const char *fmt, ...);

__END_DECLS;

#endif // __DARWIN_STDIO_H
