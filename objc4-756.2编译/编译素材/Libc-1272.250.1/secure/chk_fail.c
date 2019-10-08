/*
 * Copyright (c) 2007-2013 Apple Inc. All rights reserved.
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

#include <syslog.h>
#include <sys/sysctl.h>
#include <sys/param.h>
#include <unistd.h>
#include <stdlib.h>
#include <TargetConditionals.h>


#if !defined(PR_13085474_CHECK)
#define PR_13085474_CHECK 1

/* Some shipped applications fail this check and were tested against
 * versions of these functions that supported overlapping buffers.
 *
 * We would rather let such applications run, using the old memmove
 * implementation, than abort() because they can't use the new
 * implementation.
 */

#include <libkern/OSAtomic.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#define DYLD_OS_VERSION(major, minor, tiny) ((((major) & 0xffff) << 16) | (((minor) & 0xff) << 8) | ((tiny) & 0xff))
#if TARGET_OS_IPHONE
#define START_VERSION DYLD_OS_VERSION(7,0,0)
#else
#define START_VERSION DYLD_OS_VERSION(10,9,0)
#endif
#endif /* !PR_13085474_CHECK */

/* For PR_13085474_CHECK set, we initialize __chk_assert_no_overlap to
 * a value neither 0 or 1.  We call _dyld_register_func_for_add_image()
 * to register a callback, and use the non-one value of
 * __chk_assert_no_overlap to skip sdk version checks (but we do
 * perform overlap checks).  To detect if the main program was built
 * prior to START_VERSION, we call dyld_get_program_sdk_version(),
 * which we do before setting up the callback (since we don't need it
 * if the main program is older).
 *
 * After _dyld_register_func_for_add_image() returns, we set
 * __chk_assert_no_overlap to 1, which enables the sdk version checking
 * for subsequent loaded shared objects.  If we then find an old version,
 * we set __chk_assert_no_overlap to 0 to turn off overlap checking.
 *
 * If PR_13085474_CHECK is zero, then we never do any sdk version checking
 * and always do overlap checks.
 */
__attribute__ ((visibility ("hidden")))
uint32_t __chk_assert_no_overlap
#if PR_13085474_CHECK
                                 = 42;
#else
                                 = 1;
#endif

#if PR_13085474_CHECK
static void
__chk_assert_no_overlap_callback(const struct mach_header *mh, intptr_t vmaddr_slide __unused) {
  if (__chk_assert_no_overlap != 1) return;
  if (dyld_get_sdk_version(mh) < START_VERSION) OSAtomicAnd32(0U, &__chk_assert_no_overlap);
}
#endif

__attribute__ ((visibility ("hidden")))
void __chk_init(void) {
#if PR_13085474_CHECK
  if (dyld_get_program_sdk_version() < START_VERSION) {
    __chk_assert_no_overlap = 0;
  } else {
    _dyld_register_func_for_add_image(__chk_assert_no_overlap_callback);
    __chk_assert_no_overlap = 1;
  }
#endif
}

__attribute__ ((noreturn))
static void
__chk_fail (const char *message)
{
  syslog(LOG_CRIT, "%s", message);
  abort_report_np("%s", message);
}

__attribute__ ((visibility ("hidden")))
__attribute__ ((noreturn))
void
__chk_fail_overflow (void) {
  __chk_fail("detected buffer overflow");
}

__attribute__ ((visibility ("hidden")))
__attribute__ ((noreturn))
void
__chk_fail_overlap (void) {
  __chk_fail("detected source and destination buffer overlap");
}


__attribute__ ((visibility ("hidden")))
void
__chk_overlap (const void *_a, size_t an, const void *_b, size_t bn) {
  uintptr_t a = (uintptr_t)_a;
  uintptr_t b = (uintptr_t)_b;

  if (__builtin_expect(an == 0 || bn == 0, 0)) {
    return;
  } else if (__builtin_expect(a == b, 0)) {
    __chk_fail_overlap();
  } else if (a < b) {
    if (__builtin_expect(a + an > b, 0))
      __chk_fail_overlap();
  } else { // b < a
    if (__builtin_expect(b + bn > a, 0))
      __chk_fail_overlap();
  }
}
