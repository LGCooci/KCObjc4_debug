/**
 * IPC.h - System Starter IPC routines
 * Wilfredo Sanchez  | wsanchez@opensource.apple.com
 * Kevin Van Vechten | kevinvv@uclink4.berkeley.edu
 * $Apple$
 **
 * Copyright (c) 1999-2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 1.1 (the "License").  You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON- INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 **/

#ifndef _IPC_H_
#define _IPC_H_

#include "SystemStarter.h"

/**
 * Monitor a startup item task.  Creates a mach port and uses the
 * invalidation callback to notify system starter when the process 
 * terminates.
 **/
void MonitorStartupItem (StartupContext aStartupContext, CFMutableDictionaryRef anItem);

/**
 * Creates a named mach port and run loop source.  The name is encoded using UTF-8.
 **/
CFRunLoopSourceRef CreateIPCRunLoopSource(CFStringRef aPortName, StartupContext aStartupContext);

#endif /* _IPC_H_ */
