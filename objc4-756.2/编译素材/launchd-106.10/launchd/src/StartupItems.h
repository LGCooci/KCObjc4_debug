/**
 * StartupItems.h - Startup Item management routines
 * Wilfredo Sanchez | wsanchez@opensource.apple.com
 * Kevin Van Vechten | kevinvv@uclink4.berkeley.edu
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

#ifndef _StartupItems_H_
#define _StartupItems_H_

#include <NSSystemDirectories.h>

#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>

#include "SystemStarter.h"

#define kProvidesKey        CFSTR("Provides")
#define kRequiresKey        CFSTR("Requires")
#define kDescriptionKey     CFSTR("Description")
#define kUsesKey            CFSTR("Uses")
#define kErrorKey           CFSTR("Error")
#define kPriorityKey        CFSTR("OrderPreference")
#define kBundlePathKey      CFSTR("PathToBundle")
#define kPIDKey             CFSTR("ProcessID")
#define kDomainKey          CFSTR("Domain")


#define kErrorPermissions   CFSTR("incorrect permissions")
#define kErrorInternal      CFSTR("SystemStarter internal error")
#define kErrorReturnNonZero CFSTR("execution of Startup script failed")
#define kErrorFork          CFSTR("could not fork() StartupItem")


/*
 * Find all available startup items in NSDomains specified by aMask.
 */
CFMutableArrayRef StartupItemListCreateWithMask (NSSearchPathDomainMask aMask);

/*
 * Returns the item responsible for providing aService.
 */
CFMutableDictionaryRef StartupItemListGetProvider (CFArrayRef anItemList, CFStringRef aService);

/*
 * Creates a list of items in anItemList which depend on anItem, given anAction.
 */
CFMutableArrayRef StartupItemListCreateDependentsList (CFMutableArrayRef anItemList,
						       CFStringRef       aService  ,
						       Action            anAction  );

/*
 * Given aWaitingList of startup items, and aStatusDict describing the
 * current startup state, returns the next startup item to run, if any.
 * Returns nil if none is available.
 * The startup order depends on the dependancies between items and the
 * priorities of the items.
 * Note that this is not necessarily deterministic; if more than one
 * startup item with the same priority is ready to run, which item gets
 * returned is not specified.
 */
CFMutableDictionaryRef StartupItemListGetNext (CFArrayRef      aWaitingList,
                                               CFDictionaryRef aStatusDict ,
					       Action          anAction    );

CFMutableDictionaryRef StartupItemWithPID (CFArrayRef anItemList, pid_t aPID);
pid_t StartupItemGetPID(CFDictionaryRef anItem);

CFStringRef StartupItemGetDescription(CFMutableDictionaryRef anItem);

/*
 * Returns a list of currently executing startup items.
 */
CFArrayRef StartupItemListGetRunning(CFArrayRef anItemList);

/*
 * Returns the total number of "Provides" entries of all loaded items.
 */
CFIndex StartupItemListCountServices (CFArrayRef anItemList);


/*
 * Utility functions
 */
void RemoveItemFromWaitingList(StartupContext aStartupContext, CFMutableDictionaryRef anItem); 
void AddItemToFailedList(StartupContext aStartupContext, CFMutableDictionaryRef anItem);

/*
 * Run the startup item.
 */
int StartupItemRun   (CFMutableDictionaryRef aStatusDict, CFMutableDictionaryRef anItem, Action  anAction);
void StartupItemExit (CFMutableDictionaryRef aStatusDict, CFMutableDictionaryRef anItem, Boolean aSuccess);		     
void StartupItemSetStatus(CFMutableDictionaryRef aStatusDict, CFMutableDictionaryRef anItem, CFStringRef aServiceName, Boolean aSuccess, Boolean aReplaceFlag);

#endif /* _StartupItems_H_ */
