/**
 * System Starter main
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

#include <unistd.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <syslog.h>
#include <CoreFoundation/CoreFoundation.h>
#include <NSSystemDirectories.h>
#include "IPC.h"
#include "StartupItems.h"
#include "SystemStarter.h"
#include "SystemStarterIPC.h"

bool gDebugFlag = false;
bool gVerboseFlag = false;
bool gNoRunFlag = false;

static void     usage(void) __attribute__((noreturn));
static int      system_starter(Action anAction, const char *aService);
static void	doCFnote(void);

int 
main(int argc, char *argv[])
{
	Action          anAction = kActionStart;
	char           *aService = NULL;
	int             ch;

	while ((ch = getopt(argc, argv, "gvxirdDqn?")) != -1) {
		switch (ch) {
		case 'v':
			gVerboseFlag = true;
			break;
		case 'x':
		case 'g':
		case 'r':
		case 'q':
			break;
		case 'd':
		case 'D':
			gDebugFlag = true;
			break;
		case 'n':
			gNoRunFlag = true;
			break;
		case '?':
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 2)
		usage();

	openlog(getprogname(), LOG_PID|LOG_CONS|(gDebugFlag ? LOG_PERROR : 0), LOG_DAEMON);
	setlogmask(LOG_UPTO(LOG_NOTICE));
	if (gVerboseFlag)
		setlogmask(LOG_UPTO(LOG_INFO));
	if (gDebugFlag)
		setlogmask(LOG_UPTO(LOG_DEBUG));

	if (!gNoRunFlag && (getuid() != 0)) {
		syslog(LOG_ERR, "must be root to run");
		exit(EXIT_FAILURE);
	}

	if (argc > 0) {
		if (strcmp(argv[0], "start") == 0) {
			anAction = kActionStart;
		} else if (strcmp(argv[0], "stop") == 0) {
			anAction = kActionStop;
		} else if (strcmp(argv[0], "restart") == 0) {
			anAction = kActionRestart;
		} else {
			usage();
		}
	}

	atexit(doCFnote);

	unlink(kFixerPath);

	if (argc == 2) {
		aService = argv[1];
	} else if (!gDebugFlag && anAction != kActionStop) {
		pid_t ipwa;
		int status;

		setpriority(PRIO_PROCESS, 0, 20);
		daemon(0, 0);

		/* Too many old StartupItems had implicit dependancies on
		 * "Network" via other StartupItems that are now no-ops.
		 *
		 * SystemStarter is not on the critical path for boot up,
		 * so we'll stall here to deal with this legacy dependancy
		 * problem.
		 */
		switch ((ipwa = fork())) {
		case -1:
			syslog(LOG_WARNING, "fork(): %m");
			break;
		case 0:
			execl("/usr/sbin/ipconfig", "ipconfig", "waitall", NULL);
			syslog(LOG_WARNING, "execl(): %m");
			exit(EXIT_FAILURE);
		default:
			if (waitpid(ipwa, &status, 0) == -1) {
				syslog(LOG_WARNING, "waitpid(): %m");
				break;
			} else if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) == 0) {
					break;
				} else {
					syslog(LOG_WARNING, "ipconfig waitall exit status: %d", WEXITSTATUS(status));
				}
			} else {
				/* must have died due to signal */
				syslog(LOG_WARNING, "ipconfig waitall: %s", strsignal(WTERMSIG(status)));
			}
			break;
		}
	}

	exit(system_starter(anAction, aService));
}


/**
 * checkForActivity checks to see if any items have completed since the last invokation.
 * If not, a message is displayed showing what item(s) are being waited on.
 **/
static void 
checkForActivity(StartupContext aStartupContext)
{
	static CFIndex  aLastStatusDictionaryCount = -1;
	static CFStringRef aWaitingForString = NULL;

	if (aStartupContext && aStartupContext->aStatusDict) {
		CFIndex         aCount = CFDictionaryGetCount(aStartupContext->aStatusDict);

		if (!aWaitingForString) {
			aWaitingForString = CFSTR("Waiting for %@");
		}
		if (aLastStatusDictionaryCount == aCount) {
			CFArrayRef      aRunningList = StartupItemListGetRunning(aStartupContext->aWaitingList);
			if (aRunningList && CFArrayGetCount(aRunningList) > 0) {
				CFMutableDictionaryRef anItem = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(aRunningList, 0);
				CFStringRef     anItemDescription = StartupItemGetDescription(anItem);
				CFStringRef     aString = aWaitingForString && anItemDescription ?
				CFStringCreateWithFormat(NULL, NULL, aWaitingForString, anItemDescription) : NULL;

				if (aString) {
					CF_syslog(LOG_INFO, CFSTR("%@"), aString);
					CFRelease(aString);
				}
				if (anItemDescription)
					CFRelease(anItemDescription);
			}
			if (aRunningList)
				CFRelease(aRunningList);
		}
		aLastStatusDictionaryCount = aCount;
	}
}

/*
 * print out any error messages to the log regarding non starting StartupItems
 */
void 
displayErrorMessages(StartupContext aStartupContext)
{
	if (aStartupContext->aFailedList && CFArrayGetCount(aStartupContext->aFailedList) > 0) {
		CFIndex         anItemCount = CFArrayGetCount(aStartupContext->aFailedList);
		CFIndex         anItemIndex;


		syslog(LOG_WARNING, "The following StartupItems failed to properly start:");

		for (anItemIndex = 0; anItemIndex < anItemCount; anItemIndex++) {
			CFMutableDictionaryRef anItem = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(aStartupContext->aFailedList, anItemIndex);
			CFStringRef     anErrorDescription = CFDictionaryGetValue(anItem, kErrorKey);
			CFStringRef     anItemPath = CFDictionaryGetValue(anItem, kBundlePathKey);

			if (anItemPath) {
				CF_syslog(LOG_WARNING, CFSTR("%@"), anItemPath);
			}
			if (anErrorDescription) {
				CF_syslog(LOG_WARNING, CFSTR(" - %@"), anErrorDescription);
			} else {
				CF_syslog(LOG_WARNING, CFSTR(" - %@"), kErrorInternal);
			}
		}
	}
	if (CFArrayGetCount(aStartupContext->aWaitingList) > 0) {
		CFIndex         anItemCount = CFArrayGetCount(aStartupContext->aWaitingList);
		CFIndex         anItemIndex;

		syslog(LOG_WARNING, "The following StartupItems were not attempted due to failure of a required service:");

		for (anItemIndex = 0; anItemIndex < anItemCount; anItemIndex++) {
			CFMutableDictionaryRef anItem = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(aStartupContext->aWaitingList, anItemIndex);
			CFStringRef     anItemPath = CFDictionaryGetValue(anItem, kBundlePathKey);
			if (anItemPath) {
				CF_syslog(LOG_WARNING, CFSTR("%@"), anItemPath);
			}
		}
	}
}


static int 
system_starter(Action anAction, const char *aService_cstr)
{
	CFRunLoopSourceRef anIPCSource = NULL;
	CFStringRef     aService = NULL;
	NSSearchPathDomainMask aMask;

	if (aService_cstr)
		aService = CFStringCreateWithCString(kCFAllocatorDefault, aService_cstr, kCFStringEncodingUTF8);

	StartupContext  aStartupContext = (StartupContext) malloc(sizeof(struct StartupContextStorage));
	if (!aStartupContext) {
		syslog(LOG_ERR, "Not enough memory to allocate startup context");
		return (1);
	}
	if (gDebugFlag && gNoRunFlag)
		sleep(1);

	/**
         * Create the IPC port
         **/
	anIPCSource = CreateIPCRunLoopSource(CFSTR(kSystemStarterMessagePort), aStartupContext);
	if (anIPCSource) {
		CFRunLoopAddSource(CFRunLoopGetCurrent(), anIPCSource, kCFRunLoopCommonModes);
		CFRelease(anIPCSource);
	} else {
		syslog(LOG_ERR, "Could not create IPC bootstrap port: %s", kSystemStarterMessagePort);
		return (1);
	}

	/**
         * Get a list of Startup Items which are in /Local and /System.
         * We can't search /Network yet because the network isn't up.
         **/
	aMask = NSSystemDomainMask | NSLocalDomainMask;

	aStartupContext->aWaitingList = StartupItemListCreateWithMask(aMask);
	aStartupContext->aFailedList = NULL;
	aStartupContext->aStatusDict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
					  &kCFTypeDictionaryValueCallBacks);
	aStartupContext->aServicesCount = 0;
	aStartupContext->aRunningCount = 0;

	if (aService) {
		CFMutableArrayRef aDependentsList = StartupItemListCreateDependentsList(aStartupContext->aWaitingList, aService, anAction);

		if (aDependentsList) {
			CFRelease(aStartupContext->aWaitingList);
			aStartupContext->aWaitingList = aDependentsList;
		} else {
			CF_syslog(LOG_ERR, CFSTR("Unknown service: %@"), aService);
			return (1);
		}
	}
	aStartupContext->aServicesCount = StartupItemListCountServices(aStartupContext->aWaitingList);

	/**
         * Do the run loop
         **/
	while (1) {
		CFMutableDictionaryRef anItem = StartupItemListGetNext(aStartupContext->aWaitingList, aStartupContext->aStatusDict, anAction);

		if (anItem) {
			int             err = StartupItemRun(aStartupContext->aStatusDict, anItem, anAction);
			if (!err) {
				++aStartupContext->aRunningCount;
				MonitorStartupItem(aStartupContext, anItem);
			} else {
				/* add item to failed list */
				AddItemToFailedList(aStartupContext, anItem);

				/* Remove the item from the waiting list. */
				RemoveItemFromWaitingList(aStartupContext, anItem);
			}
		} else {
			/*
			 * If no item was selected to run, and if no items
			 * are running, startup is done.
			 */
			if (aStartupContext->aRunningCount == 0) {
				syslog(LOG_DEBUG, "none left");
				break;
			}
			/*
			 * Process incoming IPC messages and item
			 * terminations
			 */
			switch (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3.0, true)) {
			case kCFRunLoopRunTimedOut:
				checkForActivity(aStartupContext);
				break;
			case kCFRunLoopRunFinished:
				break;
			case kCFRunLoopRunStopped:
				break;
			case kCFRunLoopRunHandledSource:
				break;
			default:
				/* unknown return value */
				break;
			}
		}
	}

	/**
         * Good-bye.
         **/
	displayErrorMessages(aStartupContext);

	/* clean up  */
	if (aStartupContext->aStatusDict)
		CFRelease(aStartupContext->aStatusDict);
	if (aStartupContext->aWaitingList)
		CFRelease(aStartupContext->aWaitingList);
	if (aStartupContext->aFailedList)
		CFRelease(aStartupContext->aFailedList);

	free(aStartupContext);
	return (0);
}

void 
CF_syslog(int level, CFStringRef message,...)
{
	char            buf[8192];
	CFStringRef     cooked_msg;
	va_list         ap;

	va_start(ap, message);
	cooked_msg = CFStringCreateWithFormatAndArguments(NULL, NULL, message, ap);
	va_end(ap);

	if (CFStringGetCString(cooked_msg, buf, sizeof(buf), kCFStringEncodingUTF8))
		syslog(level, buf);

	CFRelease(cooked_msg);
}

static void 
usage(void)
{
	fprintf(stderr, "usage: %s [-vdqn?] [ <action> [ <item> ] ]\n"
	"\t<action>: action to take (start|stop|restart); default is start\n"
		"\t<item>  : name of item to act on; default is all items\n"
		"options:\n"
		"\t-v: verbose startup\n"
		"\t-d: print debugging output\n"
		"\t-q: be quiet (disable debugging output)\n"
	     "\t-n: don't actually perform action on items (pretend mode)\n"
		"\t-?: show this help\n",
		getprogname());
	exit(EXIT_FAILURE);
}

static void doCFnote(void)
{
	CFNotificationCenterPostNotificationWithOptions(
			CFNotificationCenterGetDistributedCenter(),
			CFSTR("com.apple.startupitems.completed"),
			NULL, NULL,
			kCFNotificationDeliverImmediately | kCFNotificationPostToAllSessions);
}
