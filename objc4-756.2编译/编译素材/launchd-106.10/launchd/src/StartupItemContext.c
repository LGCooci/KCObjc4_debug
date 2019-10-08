/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <servers/bootstrap.h>

int main(int argc, char *argv[])
{
	mach_port_t root_bootstrap_port;
	kern_return_t ret;
#if 0
	task_t init_task;
	int err;
#endif

	if (argc < 2) {
		fprintf(stderr, "usage: %s executable [args...]\n", argv[0]);
		exit(1);
	}

	if (geteuid() != 0) {
		fprintf(stderr, "%s: permission denied: must be run as root\n", argv[0]);
		return(EPERM);
		exit(1);
	}

#if 0
	/* get init's task port */
	ret = task_for_pid((mach_task_self)(), 1, &init_task);
	if (ret != KERN_SUCCESS) {
		fprintf(stderr, "%s: task_for_pid() failed: permission denied\n", 
				argv[0]);
		exit(1);
	}
	/* extract its bootstrap port, which should be the root */
	ret = task_get_bootstrap_port(init_task, &root_bootstrap_port);
	if (ret != KERN_SUCCESS) {
		fprintf(stderr, "%s: task_get_bootstrap_port() failed: %s\n", 
				argv[0], mach_error_string(ret));
		exit(2);
	}
#else
	/* 
	 * Keep looping, getting out bootstrap's parent until it repeats, then we
	 * know we are at the root/startupItem context.
	 */
	{
		mach_port_t cur_bootstrap;

		root_bootstrap_port = bootstrap_port;
		do {
			cur_bootstrap = root_bootstrap_port;
			ret = bootstrap_parent(cur_bootstrap, &root_bootstrap_port);
			if (ret == BOOTSTRAP_NOT_PRIVILEGED) {
				fprintf(stderr, "%s: must be run as root\n", argv[0]);
				exit(1);
			}
		} while (root_bootstrap_port != cur_bootstrap);
	}
#endif

	/* set that as our bootstrap port */
	ret = task_set_bootstrap_port(mach_task_self(), root_bootstrap_port);
	if (ret != KERN_SUCCESS) {
		fprintf(stderr, "%s: task_set_bootstrap_port() failed: %s\n", 
				argv[0], mach_error_string(ret));
		exit(3);
	}
	
	/* exec the program called for */
	execv(argv[1], &argv[1]); /* should not return */
	fprintf(stderr, "%s: exec failed: %s(%d)\n", argv[0], strerror(errno), errno);
	return(4);
}  
