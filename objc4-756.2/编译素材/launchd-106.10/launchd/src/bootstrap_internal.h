/*
 * Copyright (c) 1999-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 1.0 (the 'License').  You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License."
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * bootstrap -- fundamental service initiator and port server
 * Mike DeMoney, NeXT, Inc.
 * Copyright, 1990.  All rights reserved.
 *
 * bootstrap_internal.h -- global internal data definitions
 */

#include <mach/mach.h>
#include <mach/notify.h>
#include <sys/types.h>
#include <stdbool.h>

#define BASEPRI_USER	31	/* AOF 20/02/2002 */

#define	ANYWHERE	TRUE	/* For use with vm_allocate() */

#define DEMAND_REQUEST	MACH_NOTIFY_LAST	/* demand service messaged */

__private_extern__ void mach_start_shutdown(int);
__private_extern__ mach_port_t mach_init_init(void);
__private_extern__ void *mach_server_loop(void*);
__private_extern__ pid_t fork_with_bootstrap_port(mach_port_t);

__private_extern__ mach_port_t lookup_only_port;
__private_extern__ mach_port_t inherited_bootstrap_port;
__private_extern__ mach_port_t self_port;		/* Compatability hack */
__private_extern__ bool forward_ok;
__private_extern__ bool debugging;
__private_extern__ mach_port_t bootstrap_port_set;
__private_extern__ mach_port_t demand_port_set;
__private_extern__ mach_port_t notify_port;
__private_extern__ mach_port_t backup_port;
__private_extern__ bool canReceive(mach_port_t);
__private_extern__ bool canSend(mach_port_t);
__private_extern__ bool register_self;
__private_extern__ const char *register_name;
