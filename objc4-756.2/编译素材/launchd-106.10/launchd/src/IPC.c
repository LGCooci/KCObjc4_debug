/**
 * IPC.c - System Starter IPC routines
 * Wilfredo Sanchez  | wsanchez@opensource.apple.com
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

#include <sys/wait.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>
#include <CoreFoundation/CoreFoundation.h>
#include <syslog.h>
#include "IPC.h"
#include "StartupItems.h"
#include "SystemStarter.h"
#include "SystemStarterIPC.h"

/* Structure to pass StartupContext and anItem to the termination handler. */
typedef struct TerminationContextStorage {
	StartupContext  aStartupContext;
	CFMutableDictionaryRef anItem;
}              *TerminationContext;

/**
 * A CFMachPort invalidation callback that records the termination of
 * a startup item task.  Stops the current run loop to give system_starter
 * another go at running items.
 **/
static void 
startupItemTerminated(CFMachPortRef aMachPort, void *anInfo)
{
	TerminationContext aTerminationContext = (TerminationContext) anInfo;

	if (aMachPort) {
		mach_port_deallocate(mach_task_self(), CFMachPortGetPort(aMachPort));
	}
	if (aTerminationContext && aTerminationContext->anItem) {
		pid_t           aPID = 0;
		pid_t           rPID = 0;
		int             aStatus = 0;
		CFMutableDictionaryRef anItem = aTerminationContext->anItem;
		StartupContext  aStartupContext = aTerminationContext->aStartupContext;

		/* Get the exit status */
		if (anItem) {
			aPID = StartupItemGetPID(anItem);
			if (aPID > 0)
				rPID = waitpid(aPID, &aStatus, 0);
		}
		if (aStartupContext) {
			--aStartupContext->aRunningCount;

			/* Record the item's status */
			if (aStartupContext->aStatusDict) {
				StartupItemExit(aStartupContext->aStatusDict, anItem, (WIFEXITED(aStatus) && WEXITSTATUS(aStatus) == 0));
				if (aStatus) {
					CF_syslog(LOG_WARNING, CFSTR("%@ (%d) did not complete successfully"), CFDictionaryGetValue(anItem, CFSTR("Description")), aPID);
				} else {
					CF_syslog(LOG_DEBUG, CFSTR("Finished %@ (%d)"), CFDictionaryGetValue(anItem, CFSTR("Description")), aPID);
				}
			}
			/*
			 * If the item failed to start, then add it to the
			 * failed list
			 */
			if (WEXITSTATUS(aStatus) || WTERMSIG(aStatus) || WCOREDUMP(aStatus)) {
				CFDictionarySetValue(anItem, kErrorKey, kErrorReturnNonZero);
				AddItemToFailedList(aStartupContext, anItem);
			}
			/*
			 * Remove the item from the waiting list regardless
			 * if it was successful or it failed.
			 */
			RemoveItemFromWaitingList(aStartupContext, anItem);
		}
	}
	if (aTerminationContext)
		free(aTerminationContext);
}

void 
MonitorStartupItem(StartupContext aStartupContext, CFMutableDictionaryRef anItem)
{
	pid_t           aPID = StartupItemGetPID(anItem);
	if (anItem && aPID > 0) {
		mach_port_t     aPort;
		kern_return_t   aResult;
		CFMachPortContext aContext;
		CFMachPortRef   aMachPort;
		CFRunLoopSourceRef aSource;
		TerminationContext aTerminationContext = (TerminationContext) malloc(sizeof(struct TerminationContextStorage));

		aTerminationContext->aStartupContext = aStartupContext;
		aTerminationContext->anItem = anItem;

		aContext.version = 0;
		aContext.info = aTerminationContext;
		aContext.retain = 0;
		aContext.release = 0;

		if ((aResult = task_for_pid(mach_task_self(), aPID, &aPort)) != KERN_SUCCESS)
			goto out_bad;

		if (!(aMachPort = CFMachPortCreateWithPort(NULL, aPort, NULL, &aContext, NULL)))
			goto out_bad;

		if (!(aSource = CFMachPortCreateRunLoopSource(NULL, aMachPort, 0))) {
			CFRelease(aMachPort);
			goto out_bad;
		}
		CFMachPortSetInvalidationCallBack(aMachPort, startupItemTerminated);
		CFRunLoopAddSource(CFRunLoopGetCurrent(), aSource, kCFRunLoopCommonModes);
		CFRelease(aSource);
		CFRelease(aMachPort);
		return;
out_bad:
		/*
		 * The assumption is something failed, the task already
		 * terminated.
		 */
		startupItemTerminated(NULL, aTerminationContext);
	}
}

/**
 * Returns a reference to an item based on tokens passed in an IPC message.
 * This is useful for figuring out which item the message came from.
 *
 * Currently two tokens are supported:
 * kIPCProcessIDKey - the pid of the running startup script
 * kIPCServiceNameKey - a name of a service that the item provides.  This
 *	takes precedence over the pid key when both are present.
 **/
static CFMutableDictionaryRef 
itemFromIPCMessage(StartupContext aStartupContext, CFDictionaryRef anIPCMessage)
{
	CFMutableDictionaryRef anItem = NULL;

	if (aStartupContext && anIPCMessage) {
		CFStringRef     aServiceName = CFDictionaryGetValue(anIPCMessage, kIPCServiceNameKey);
		CFIndex         aPID = 0;
		CFNumberRef     aPIDNumber = CFDictionaryGetValue(anIPCMessage, kIPCProcessIDKey);

		if (aServiceName && CFGetTypeID(aServiceName) == CFStringGetTypeID()) {
			anItem = StartupItemListGetProvider(aStartupContext->aWaitingList, aServiceName);
		} else if (aPIDNumber &&
			   CFGetTypeID(aPIDNumber) == CFNumberGetTypeID() &&
		CFNumberGetValue(aPIDNumber, kCFNumberCFIndexType, &aPID)) {
			anItem = StartupItemWithPID(aStartupContext->aWaitingList, aPID);
		}
	}
	return anItem;
}

/**
 * Displays a message on the console.
 * aConsoleMessage will be localized according to the dictionary in the specified item.
 * Running tems may be specified by their current process id.  Items may also be specified
 * by one of the service names they provide.
 **/
static void 
consoleMessage(StartupContext aStartupContext, CFDictionaryRef anIPCMessage)
{
	if (aStartupContext && anIPCMessage) {
		CFStringRef     aConsoleMessage = CFDictionaryGetValue(anIPCMessage, kIPCConsoleMessageKey);

		if (aConsoleMessage && CFGetTypeID(aConsoleMessage) == CFStringGetTypeID()) {
			CF_syslog(LOG_INFO, CFSTR("%@"), aConsoleMessage);
		}
	}
}

/**
 * Records the success or failure or a particular service.
 * If no service name is specified, but a pid is, then all services provided
 * by the item are flagged.
 **/
static void 
statusMessage(StartupContext aStartupContext, CFDictionaryRef anIPCMessage)
{
	if (anIPCMessage && aStartupContext && aStartupContext->aStatusDict) {
		CFMutableDictionaryRef anItem = itemFromIPCMessage(aStartupContext, anIPCMessage);
		CFStringRef     aServiceName = CFDictionaryGetValue(anIPCMessage, kIPCServiceNameKey);
		CFBooleanRef    aStatus = CFDictionaryGetValue(anIPCMessage, kIPCStatusKey);

		if (anItem && aStatus &&
		    CFGetTypeID(aStatus) == CFBooleanGetTypeID() &&
		    (!aServiceName || CFGetTypeID(aServiceName) == CFStringGetTypeID())) {
			StartupItemSetStatus(aStartupContext->aStatusDict, anItem, aServiceName, CFBooleanGetValue(aStatus), TRUE);
		}
	}
}

/**
 * Queries one of several configuration settings.
 */
static CFDataRef 
queryConfigSetting(StartupContext aStartupContext, CFDictionaryRef anIPCMessage)
{
	char           *aValue = "";

	if (anIPCMessage) {
		CFStringRef     aSetting = CFDictionaryGetValue(anIPCMessage, kIPCConfigSettingKey);

		if (aSetting && CFGetTypeID(aSetting) == CFStringGetTypeID()) {
			if (CFEqual(aSetting, kIPCConfigSettingVerboseFlag)) {
				aValue = gVerboseFlag ? "-YES-" : "-NO-";
			} else if (CFEqual(aSetting, kIPCConfigSettingNetworkUp)) {
				Boolean         aNetworkUpFlag = FALSE;
				if (aStartupContext && aStartupContext->aStatusDict) {
					aNetworkUpFlag = CFDictionaryContainsKey(aStartupContext->aStatusDict, CFSTR("Network"));
				}
				aValue = aNetworkUpFlag ? "-YES-" : "-NO-";
			}
		}
	}
	return CFDataCreate(NULL, (const UInt8 *)aValue, strlen(aValue) + 1);	/* aValue + null */
}

static void    *handleIPCMessage(void *aMsgParam, CFIndex aMessageSize __attribute__((unused)), CFAllocatorRef anAllocator __attribute__((unused)), void *aMachPort) {
	SystemStarterIPCMessage *aMessage = (SystemStarterIPCMessage *) aMsgParam;
	SystemStarterIPCMessage *aReplyMessage = NULL;

	CFDataRef       aResult = NULL;
	CFDataRef       aData = NULL;

	if (aMessage->aHeader.msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		syslog(LOG_WARNING, "Ignoring out-of-line IPC message");
		return NULL;
	} else {
		mach_msg_security_trailer_t *aSecurityTrailer = (mach_msg_security_trailer_t *)
		((uint8_t *) aMessage + round_msg(sizeof(SystemStarterIPCMessage) + aMessage->aByteLength));

		/*
		 * CFRunLoop includes the format 0 message trailer with the
		 * passed message.
		 */
		if (aSecurityTrailer->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 &&
		    aSecurityTrailer->msgh_sender.val[0] != 0) {
			syslog(LOG_WARNING, "Ignoring IPC message sent from uid %d", aSecurityTrailer->msgh_sender.val[0]);
			return NULL;
		}
	}

	if (aMessage->aProtocol != kIPCProtocolVersion) {
		syslog(LOG_WARNING, "Unsupported IPC protocol version number: %d.  Message ignored", aMessage->aProtocol);
		return NULL;
	}
	aData = CFDataCreateWithBytesNoCopy(NULL,
		     (uint8_t *) aMessage + sizeof(SystemStarterIPCMessage),
					    aMessage->aByteLength,
					    kCFAllocatorNull);
	/*
         * Dispatch the IPC message.
         */
	if (aData) {
		StartupContext  aStartupContext = NULL;
		CFStringRef     anErrorString = NULL;
		CFDictionaryRef anIPCMessage = (CFDictionaryRef) CFPropertyListCreateFromXMLData(NULL, aData, kCFPropertyListImmutable, &anErrorString);

		CF_syslog(LOG_DEBUG, CFSTR("IPC message = %@"), anIPCMessage);

		if (aMachPort) {
			CFMachPortContext aMachPortContext;
			CFMachPortGetContext((CFMachPortRef) aMachPort, &aMachPortContext);
			aStartupContext = (StartupContext) aMachPortContext.info;
		}
		if (anIPCMessage && CFGetTypeID(anIPCMessage) == CFDictionaryGetTypeID()) {
			/* switch on the type of the IPC message */
			CFStringRef     anIPCMessageType = CFDictionaryGetValue(anIPCMessage, kIPCMessageKey);
			if (anIPCMessageType && CFGetTypeID(anIPCMessageType) == CFStringGetTypeID()) {
				if (CFEqual(anIPCMessageType, kIPCConsoleMessage)) {
					consoleMessage(aStartupContext, anIPCMessage);
				} else if (CFEqual(anIPCMessageType, kIPCStatusMessage)) {
					statusMessage(aStartupContext, anIPCMessage);
				} else if (CFEqual(anIPCMessageType, kIPCQueryMessage)) {
					aResult = queryConfigSetting(aStartupContext, anIPCMessage);
				}
			}
		} else {
			CF_syslog(LOG_ERR, CFSTR("Unable to parse IPC message: %@"), anErrorString);
		}
		CFRelease(aData);
	} else {
		syslog(LOG_ERR, "Out of memory.  Could not allocate space for IPC message");
	}

	/*
         * Generate a Mach message for the result data.
         */
	if (!aResult)
		aResult = CFDataCreateWithBytesNoCopy(NULL, (const UInt8 *)"", 1, kCFAllocatorNull);
	if (aResult) {
		CFIndex         aDataSize = CFDataGetLength(aResult);
		CFIndex         aReplyMessageSize = round_msg(sizeof(SystemStarterIPCMessage) + aDataSize + 3);
		aReplyMessage = CFAllocatorAllocate(kCFAllocatorSystemDefault, aReplyMessageSize, 0);
		if (aReplyMessage) {
			aReplyMessage->aHeader.msgh_id = -1 * (SInt32) aMessage->aHeader.msgh_id;
			aReplyMessage->aHeader.msgh_size = aReplyMessageSize;
			aReplyMessage->aHeader.msgh_remote_port = aMessage->aHeader.msgh_remote_port;
			aReplyMessage->aHeader.msgh_local_port = MACH_PORT_NULL;
			aReplyMessage->aHeader.msgh_reserved = 0;
			aReplyMessage->aHeader.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
			aReplyMessage->aBody.msgh_descriptor_count = 0;
			aReplyMessage->aProtocol = kIPCProtocolVersion;
			aReplyMessage->aByteLength = CFDataGetLength(aResult);
			memmove((uint8_t *) aReplyMessage + sizeof(SystemStarterIPCMessage),
				CFDataGetBytePtr(aResult),
				CFDataGetLength(aResult));
		}
		CFRelease(aResult);
	}
	if (!aReplyMessage) {
		syslog(LOG_ERR, "Out of memory.  Could not allocate IPC result");
	}
	return aReplyMessage;
}


static mach_port_t 
getIPCPort(void *anInfo)
{
	return anInfo ? CFMachPortGetPort((CFMachPortRef) anInfo) : MACH_PORT_NULL;
}

CFRunLoopSourceRef 
CreateIPCRunLoopSource(CFStringRef aPortName, StartupContext aStartupContext)
{
	CFRunLoopSourceRef aSource = NULL;
	CFMachPortRef   aMachPort = NULL;
	CFMachPortContext aContext;
	kern_return_t   aKernReturn = KERN_FAILURE;

	aContext.version = 0;
	aContext.info = (void *) aStartupContext;
	aContext.retain = 0;
	aContext.release = 0;
	aContext.copyDescription = 0;
	aMachPort = CFMachPortCreate(NULL, NULL, &aContext, NULL);

	if (aMachPort && aPortName) {
		CFIndex         aPortNameLength = CFStringGetLength(aPortName);
		CFIndex         aPortNameSize = CFStringGetMaximumSizeForEncoding(aPortNameLength, kCFStringEncodingUTF8) + 1;
		char            *aBuffer = CFAllocatorAllocate(NULL, aPortNameSize, 0);
		if (aBuffer && CFStringGetCString(aPortName,
						  aBuffer,
						  aPortNameSize,
						  kCFStringEncodingUTF8)) {
			mach_port_t     aBootstrapPort;
			task_get_bootstrap_port(mach_task_self(), &aBootstrapPort);
			aKernReturn = bootstrap_register(aBootstrapPort, aBuffer, CFMachPortGetPort(aMachPort));
		}
		if (aBuffer)
			CFAllocatorDeallocate(NULL, aBuffer);
	}
	if (aMachPort && aKernReturn == KERN_SUCCESS) {
		CFRunLoopSourceContext1 aSourceContext;
		aSourceContext.version = 1;
		aSourceContext.info = aMachPort;
		aSourceContext.retain = CFRetain;
		aSourceContext.release = CFRelease;
		aSourceContext.copyDescription = CFCopyDescription;
		aSourceContext.equal = CFEqual;
		aSourceContext.hash = CFHash;
		aSourceContext.getPort = getIPCPort;
		aSourceContext.perform = (void *) handleIPCMessage;
		aSource = CFRunLoopSourceCreate(NULL, 0, (CFRunLoopSourceContext *) & aSourceContext);
	}
	if (aMachPort && (!aSource || aKernReturn != KERN_SUCCESS)) {
		CFMachPortInvalidate(aMachPort);
		CFRelease(aMachPort);
		aMachPort = NULL;
	}
	return aSource;
}
