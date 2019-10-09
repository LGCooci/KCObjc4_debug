/**
 * SystemStarterIPC.h - System Starter IPC definitions
 * Wilfredo Sanchez  | wsanchez@opensource.apple.com
 * Kevin Van Vechten | kevinvv@uclink4.berkeley.edu
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
 **
 * Definitions used for IPC communications with SystemStarter.
 * SystemStarter listens on a CFMessagePort with the name defined by
 * kSystemStarterMessagePort.  The messageID of each message should
 * be set to the kIPCProtocolVersion constant.  The contents of each
 * message should be an XML plist containing a dictionary using
 * the keys defined in this file.
 **/

#ifndef _SYSTEM_STARTER_IPC_H
#define _SYSTEM_STARTER_IPC_H

#include <CoreFoundation/CFString.h>
#include <mach/message.h>

/* Compatible with inline CFMessagePort messages. */
typedef struct SystemStarterIPCMessage {
    mach_msg_header_t aHeader;
    mach_msg_body_t   aBody;
    SInt32            aProtocol;
    SInt32            aByteLength;
    /* Data follows. */
} SystemStarterIPCMessage;

/* Name of the CFMessagePort SystemStarter listens on. */
#define kSystemStarterMessagePort	"com.apple.SystemStarter"

/* kIPCProtocolVersion should be passed as the messageID of the CFMessage. */
#define kIPCProtocolVersion		0

/* kIPCTypeKey should be provided for all messages. */
#define kIPCMessageKey			CFSTR("Message")

/* Messages are one of the following types: */
#define kIPCConsoleMessage		CFSTR("ConsoleMessage")
#define kIPCStatusMessage		CFSTR("StatusMessage")
#define kIPCQueryMessage		CFSTR("QueryMessage")
#define kIPCLoadDisplayBundleMessage	CFSTR("LoadDisplayBundle")
#define kIPCUnloadDisplayBundleMessage	CFSTR("UnloadDisplayBundle")

/* kIPCServiceNameKey identifies a startup item by one of the services it provides. */
#define kIPCServiceNameKey		CFSTR("ServiceName")

/* kIPCProcessIDKey identifies a running startup item by its process id. */
#define kIPCProcessIDKey		CFSTR("ProcessID")

/* kIPCConsoleMessageKey contains the non-localized string to
 * display for messages of type kIPCTypeConsoleMessage.
 */
#define kIPCConsoleMessageKey		CFSTR("ConsoleMessage")

/* kIPCStatus key contains a boolean value.  True for success, false for failure. */
#define kIPCStatusKey			CFSTR("StatusKey")

/* kIPCDisplayBundlePathKey contains a string path to the display bundle
   SystemStarter should attempt to load. */
#define kIPCDisplayBundlePathKey	CFSTR("DisplayBundlePath")

/* kIPCConfigNamegKey contains the name of a config setting to query */
#define kIPCConfigSettingKey		CFSTR("ConfigSetting") 

/* Some config settings */
#define kIPCConfigSettingVerboseFlag	CFSTR("VerboseFlag")
#define kIPCConfigSettingNetworkUp	CFSTR("NetworkUp")

#endif /* _SYSTEM_STARTER_IPC_H */
