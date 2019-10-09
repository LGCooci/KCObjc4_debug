/**
 * ConsoleMessage.c - ConsoleMessage main
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
 **
 * The ConsoleMessage utility sends an IPC message to SystemStarter
 * containing the message specified on the command line.  SystemStarter
 * will perform the localization.  The message is also printed to
 * the system log.
 **/

#include <unistd.h>
#include <crt_externs.h>
#include <syslog.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>
#include <CoreFoundation/CoreFoundation.h>
#include "SystemStarterIPC.h"

static CFDataRef sendIPCMessage(CFStringRef aPortName, CFDataRef aData, CFStringRef aRunLoopMode);

static void     usage() __attribute__((__noreturn__));
static void 
usage()
{
	/* char* aProgram = **_NSGetArgv(); */
	fprintf(stderr, "usage:\n"
		"\tConsoleMessage [-v] <message>\n"
		"\tConsoleMessage [-v] -S\n"
		"\tConsoleMessage [-v] -F\n"
		"\tConsoleMessage [-v] -s <service>\n"
		"\tConsoleMessage [-v] -f <service>\n"
		"\tConsoleMessage [-v] -q <setting>\n"
		"\tConsoleMessage [-v] -b <path>\n"
		"\tConsoleMessage [-v] -u\n"
		"\noptions:\n"
		"\t-v: verbose (prints errors to stdout)\n"
		"\t-S: mark all services as successful\n"
		"\t-F: mark all services as failed\n"
		"\t-s: mark a specific service as successful\n"
		"\t-f: mark a specific service as failed\n"
		"\t-q: query a configuration setting\n"
		"\t-b: (ignored)\n"
		"\t-u: (ignored)\n");
	exit(1);
}

enum {
	kActionConsoleMessage,
	kActionSuccess,
	kActionFailure,
	kActionQuery,
};

int 
main(int argc, char *argv[])
{
	int             anExitCode = 0;
	int             aVerboseFlag = 0;
	int             anAction = kActionConsoleMessage;
	char           *aProgram = argv[0];
	char           *anArgCStr = NULL;
	char            c;
	pid_t		w4lw_pid = 0;
	FILE	       *w4lw_f;

	/**
         * Handle command line.
         **/
	while ((c = getopt(argc, argv, "?vSFs:f:q:b:u")) != -1) {
		switch (c) {
		case '?':
			usage();
			break;
		case 'v':
			aVerboseFlag = 1;
			break;
		case 'S':
			anAction = kActionSuccess;
			anArgCStr = NULL;
			break;
		case 'F':
			anAction = kActionFailure;
			anArgCStr = NULL;
			break;
		case 's':
			anAction = kActionSuccess;
			anArgCStr = optarg;
			break;
		case 'f':
			anAction = kActionFailure;
			anArgCStr = optarg;
			break;
		case 'q':
			anAction = kActionQuery;
			anArgCStr = optarg;
			break;
		case 'b':
			exit(EXIT_SUCCESS);
			break;
		case 'u':
			w4lw_f = fopen("/var/run/waiting4loginwindow.pid", "r");
			if (w4lw_f) {
				fscanf(w4lw_f, "%d\n", &w4lw_pid);
				if (w4lw_pid)
					kill(w4lw_pid, SIGTERM);
			}
			exit(EXIT_SUCCESS);
			break;
		default:
			fprintf(stderr, "ignoring unknown option '-%c'\n", c);
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if ((anAction == kActionConsoleMessage && argc != 1) ||
	    (anAction == kActionSuccess && argc != 0) ||
	    (anAction == kActionFailure && argc != 0) ||
	    (anAction == kActionQuery && argc != 0)) {
		usage();
	}
	if (getuid() != 0) {
		fprintf(stderr, "you must be root to run %s\n", aProgram);
		exit(1);
	} else {
		CFMutableDictionaryRef anIPCMessage = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
					  &kCFTypeDictionaryValueCallBacks);

		if (anIPCMessage) {
			CFStringRef     anArg = NULL;
			CFIndex         aPID = getppid();
			CFNumberRef     aPIDNumber = CFNumberCreate(NULL, kCFNumberCFIndexType, &aPID);

			/*
			 * Parent process id is the process id of the startup
			 * item that called ConsoleMessage.
			 */
			CFDictionarySetValue(anIPCMessage, kIPCProcessIDKey, aPIDNumber);
			CFRelease(aPIDNumber);

			if (anArgCStr) {
				anArg = CFStringCreateWithCString(NULL, anArgCStr, kCFStringEncodingUTF8);
			}
			if (anAction == kActionSuccess || anAction == kActionFailure) {
				CFBooleanRef    aStatus = (anAction == kActionSuccess) ? kCFBooleanTrue : kCFBooleanFalse;
				CFDictionarySetValue(anIPCMessage, kIPCMessageKey, kIPCStatusMessage);
				CFDictionarySetValue(anIPCMessage, kIPCStatusKey, aStatus);
				if (anArg)
					CFDictionarySetValue(anIPCMessage, kIPCServiceNameKey, anArg);
			} else if (anAction == kActionQuery && anArg) {
				CFDictionarySetValue(anIPCMessage, kIPCMessageKey, kIPCQueryMessage);
				CFDictionarySetValue(anIPCMessage, kIPCConfigSettingKey, anArg);
			} else if (anAction == kActionConsoleMessage) {
				char           *aConsoleMessageCStr = argv[0];
				CFStringRef     aConsoleMessage = CFStringCreateWithCString(NULL, aConsoleMessageCStr, kCFStringEncodingUTF8);

				syslog(LOG_INFO, "%s", aConsoleMessageCStr);

				CFDictionarySetValue(anIPCMessage, kIPCMessageKey, kIPCConsoleMessage);
				CFDictionarySetValue(anIPCMessage, kIPCConsoleMessageKey, aConsoleMessage);
				CFRelease(aConsoleMessage);
			}
			if (anArg)
				CFRelease(anArg);

			{
				CFDataRef       aData = CFPropertyListCreateXMLData(NULL, anIPCMessage);
				if (aData) {
					CFDataRef       aResultData = sendIPCMessage(CFSTR(kSystemStarterMessagePort), aData, kCFRunLoopDefaultMode);

					/* aResultData should be ASCIZ */
					if (aResultData) {
						fprintf(stdout, "%s", CFDataGetBytePtr(aResultData));
						CFRelease(aResultData);
					} else {
						char           *aConsoleMessageCStr = argv[0];
						fprintf(stdout, "%s\n", aConsoleMessageCStr);

						if (aVerboseFlag)
							fprintf(stderr, "%s could not connect to SystemStarter.\n", aProgram);
						anExitCode = 0;
					}
					CFRelease(aData);
				} else {
					if (aVerboseFlag)
						fprintf(stderr, "%s: not enough memory to create IPC message.\n", aProgram);
					anExitCode = 1;
				}
			}
		} else {
			if (aVerboseFlag)
				fprintf(stderr, "%s: not enough memory to create IPC message.\n", aProgram);
			anExitCode = 1;
		}
	}
	exit(anExitCode);
}


static void 
dummyCallback(void)
{
}

static void     replyCallback(CFMachPortRef port __attribute__((unused)), void *aPtr, CFIndex aSize __attribute__((unused)), CFDataRef * aReply) {
	SystemStarterIPCMessage *aMessage = (SystemStarterIPCMessage *) aPtr;

	if (aReply != NULL &&
	    aMessage->aProtocol == kIPCProtocolVersion &&
	    aMessage->aByteLength >= 0) {
		*aReply = CFDataCreate(NULL, (UInt8 *) aMessage + aMessage->aByteLength, aMessage->aByteLength);
	} else if (aReply != NULL) {
		*aReply = NULL;
	}
}


static CFDataRef 
sendIPCMessage(CFStringRef aPortName, CFDataRef aData, CFStringRef aRunLoopMode)
{
	SystemStarterIPCMessage *aMessage = NULL;
	CFRunLoopSourceRef aSource = NULL;
	CFMachPortRef   aMachPort = NULL, aReplyPort = NULL;
	CFMachPortContext aContext;
	kern_return_t   aKernReturn = KERN_FAILURE;
	mach_port_t     aBootstrapPort, aNativePort;
	char           *aPortNameUTF8String;
	CFDataRef       aReply = NULL;
	SInt32          aStrLen;

	aContext.version = 0;
	aContext.info = (void *) NULL;
	aContext.retain = 0;
	aContext.release = 0;
	aContext.copyDescription = 0;

	/* Look up the remote port by name */

	aStrLen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(aPortName) + 1, kCFStringEncodingUTF8);
	aPortNameUTF8String = malloc(aStrLen);
	if (aPortNameUTF8String) {
		CFStringGetCString(aPortName, aPortNameUTF8String, aStrLen, kCFStringEncodingUTF8);
		task_get_bootstrap_port(mach_task_self(), &aBootstrapPort);
		aKernReturn = bootstrap_look_up(aBootstrapPort, aPortNameUTF8String, &aNativePort);
		aMachPort = (KERN_SUCCESS == aKernReturn) ? CFMachPortCreateWithPort(NULL, aNativePort, (CFMachPortCallBack) dummyCallback, &aContext, NULL) : NULL;
		free(aPortNameUTF8String);
	}
	/* Create a reply port and associated run loop source */
	aContext.info = &aReply;
	aReplyPort = CFMachPortCreate(NULL, (CFMachPortCallBack) replyCallback, &aContext, NULL);
	if (aReplyPort) {
		aSource = CFMachPortCreateRunLoopSource(NULL, aReplyPort, 0);
		if (aSource) {
			CFRunLoopAddSource(CFRunLoopGetCurrent(), aSource, aRunLoopMode);
		}
	}
	/* Allocate a buffer for the message */
	if (aData && aMachPort && aReplyPort) {
		SInt32          aSize = (sizeof(SystemStarterIPCMessage) + (CFDataGetLength(aData) + 3)) & ~0x3;
		aMessage = (SystemStarterIPCMessage *) malloc(aSize);
		if (aMessage) {
			aMessage->aHeader.msgh_id = 1;
			aMessage->aHeader.msgh_size = aSize;
			aMessage->aHeader.msgh_remote_port = CFMachPortGetPort(aMachPort);
			aMessage->aHeader.msgh_local_port = CFMachPortGetPort(aReplyPort);
			aMessage->aHeader.msgh_reserved = 0;
			aMessage->aHeader.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
			aMessage->aBody.msgh_descriptor_count = 0;
			aMessage->aProtocol = kIPCProtocolVersion;
			aMessage->aByteLength = CFDataGetLength(aData);
			memmove((uint8_t *) aMessage + sizeof(SystemStarterIPCMessage),
				CFDataGetBytePtr(aData),
				CFDataGetLength(aData));
		}
	}
	/* Wait up to 1 second to send the message */
	if (aMessage) {
		aKernReturn = mach_msg((mach_msg_header_t *) aMessage, MACH_SEND_MSG | MACH_SEND_TIMEOUT, aMessage->aHeader.msgh_size, 0, MACH_PORT_NULL, 1000.0, MACH_PORT_NULL);
		free(aMessage);
	}
	/* Wait up to 30 seconds for the reply */
	if (aSource && aKernReturn == MACH_MSG_SUCCESS) {
		CFRetain(aReplyPort);
		CFRunLoopRunInMode(aRunLoopMode, 30.0, true);
		/*
		 * aReplyPort's replyCallback will set the local aReply
		 * variable
		 */
		CFRelease(aReplyPort);
	}
	if (aMachPort)
		CFRelease(aMachPort);
	if (aReplyPort)
		CFRelease(aReplyPort);
	if (aSource) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), aSource, aRunLoopMode);
		CFRelease(aSource);
	}
	return aReply;
}
