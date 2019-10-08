/**
 * SystemStarter.h - System Starter driver
 * Wilfredo Sanchez | wsanchez@opensource.apple.com
 * $Apple$
 **
 * Copyright (c) 1999-2002 Apple Computer, Inc. All rights reserved.
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

#ifndef _SYSTEM_STARTER_H_
#define _SYSTEM_STARTER_H_

/* Structure to pass common objects from system_starter to the IPC handlers */
typedef struct StartupContextStorage {
    CFMutableArrayRef           aWaitingList;
    CFMutableArrayRef           aFailedList;
    CFMutableDictionaryRef      aStatusDict;
    int                         aServicesCount;
    int                         aRunningCount;
} *StartupContext;

#define kFixerDir	"/var/db/fixer"
#define kFixerPath	"/var/db/fixer/StartupItems"

/* Action types */
typedef enum {
  kActionNone = 0,
  kActionStart,
  kActionStop,
  kActionRestart
} Action;

void CF_syslog(int level, CFStringRef message, ...);
extern bool gVerboseFlag;

#endif /* _SYSTEM_STARTER_H_ */
