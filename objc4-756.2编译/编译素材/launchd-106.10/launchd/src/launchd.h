/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
#ifndef __LAUNCHD_H__
#define __LAUNCHD_H__

#include <sys/event.h>

typedef void (*kq_callback)(void *, struct kevent *);

extern kq_callback kqsimple_zombie_reaper;
extern mach_port_t launchd_bootstrap_port;
extern sigset_t blocked_signals;

#ifdef PID1_REAP_ADOPTED_CHILDREN
extern int pid1_child_exit_status;
#endif

int kevent_mod(uintptr_t ident, short filter, u_short flags, u_int fflags, intptr_t data, void *udata);
void launchd_SessionCreate(const char *who);

void init_boot(bool sflag, bool vflag, bool xflag);
void init_pre_kevent(void);
bool init_check_pid(pid_t);

void update_ttys(void);
void catatonia(void);
void death(void);

#endif
