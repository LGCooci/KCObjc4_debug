/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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
 * libdarwin internal header.
 */
#ifndef __DARWIN_INTERNAL_H
#define __DARWIN_INTERNAL_H

#include <os/base.h>
#include <os/api.h>
#include <Availability.h>

#include <mach/port.h>
#include <mach/message.h>
#include <mach/host_priv.h>
#include <mach/host_reboot.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/port.h>
#include <mach/message.h>
#include <mach/host_priv.h>
#include <mach/host_reboot.h>

#include <sys/sysctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/errno.h>
#include <sys/paths.h>
#include <sys/spawn.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/errno.h>
#include <sys/paths.h>
#include <sys/spawn.h>
#include <sys/proc_info.h>

#define OS_CRASH_ENABLE_EXPERIMENTAL_LIBTRACE 1
#include <os/assumes.h>
#include <os/transaction_private.h>
#include <os/log_private.h>
#include <os/alloc_once_private.h>

#include <mach-o/getsect.h>
#include <bsm/libbsm.h>
#include <sysexits.h>
#include <spawn.h>
#include <libproc.h>
#include <string.h>
#include <dlfcn.h>
#include <err.h>
#include <ctype.h>
#include <struct.h>
#include <bootstrap_priv.h>
#include <assert.h>

#define RDAR_12809455 1

#include "h/bsd.h"
#include "h/cleanup.h"
#include "h/err.h"
#include "h/errno.h"
#include "h/mach_exception.h"
#include "h/mach_utils.h"
#include "h/stdio.h"
#include "h/stdlib.h"
#include "h/string.h"

#if DARWIN_TAPI
// Duplicate declarations to make TAPI happy. This header is included in the
// TAPI build as an extra public header.
API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
OS_EXPORT OS_NONNULL1
void
os_assert_mach(const char *op, kern_return_t kr);

API_AVAILABLE(macos(10.14), ios(12.0), tvos(12.0), watchos(5.0))
OS_EXPORT
void
os_assert_mach_port_status(const char *desc, mach_port_t p,
		mach_port_status_t *expected);
#endif

#endif //__DARWIN_INTERNAL_H
