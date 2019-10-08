/*
 * Copyright (c) 1999-2004 Apple Computer, Inc. All rights reserved.
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
 * lists.h -- interface to list routines
 */

#include <sys/types.h>
#include <mach/mach.h>
#include <mach/boolean.h>
#include <servers/bootstrap_defs.h>

#include <bsm/audit.h>

#ifndef NULL
#define	NULL	((void *)0)
#endif NULL

typedef struct bootstrap bootstrap_info_t;
typedef struct service service_t;
typedef struct server server_t;

/* Bootstrap info */
struct bootstrap {
	bootstrap_info_t		*next;		/* list of all bootstraps */
	bootstrap_info_t		*prev;
	bootstrap_info_t		*parent;
	bootstrap_info_t		*deactivate;	/* list being deactivated */
	mach_port_name_t		bootstrap_port;
	mach_port_name_t		requestor_port;
	unsigned int			ref_count;
};

/* Service types */
typedef enum {
	DECLARED,	/* Declared in config file */
	REGISTERED	/* Registered dynamically */
} servicetype_t;

struct service {
	service_t	*next;		/* list of all services */
	service_t	*prev;
	name_t		name;		/* service name */
	mach_port_name_t	port;		/* service port,
					   may have all rights if inactive */
	bootstrap_info_t	*bootstrap;	/* bootstrap port(s) used at this
					 * level. */
	boolean_t	isActive;	/* server is running */
	servicetype_t	servicetype;	/* Declared, Registered, or Machport */
	server_t	*server;	/* server, declared services only */
};

/* Server types */
typedef enum {
	SERVER,		/* Launchable server */
	RESTARTABLE,	/* Restartable server */
	DEMAND,		/* Restartable server - on demand */
	MACHINIT,	/* mach_init doesn't get launched. */
} servertype_t;

#define	NULL_SERVER	NULL
#define	ACTIVE		TRUE

struct server {
	server_t	*next;		/* list of all servers */
	server_t	*prev;
	servertype_t	servertype;
	cmd_t		cmd;		/* server command to exec */
	uid_t		uid;		/* uid to exec server with */
	auditinfo_t	auinfo;		/* server's audit information */
	mach_port_t	port;		/* server's priv bootstrap port */
	mach_port_t	task_port;	/* server's task port */
	pid_t		pid;		/* server's pid */
	int		activity;		/* count of checkins/registers this instance */
	int		active_services;/* count of active services */
	bootstrap_info_t *bootstrap; /* bootstrap context */
};

#define	NO_PID	(-1)

extern void init_lists(void);

extern server_t *new_server(
	bootstrap_info_t 	*bootstrap,
	const char			*cmd,
	uid_t					uid,
	servertype_t		servertype,
	auditinfo_t		auinfo);

extern service_t 		*new_service(
	bootstrap_info_t	*bootstrap,
	const char			*name,
	mach_port_t			serviceport,
	boolean_t			isActive,
	servicetype_t		servicetype,
	server_t			*serverp);

extern bootstrap_info_t *new_bootstrap(
	bootstrap_info_t	*parent,
	mach_port_name_t	bootstrapport,
	mach_port_name_t	requestorport);

extern server_t *lookup_server_by_port(mach_port_t port);
extern server_t *lookup_server_by_task_port(mach_port_t port);
extern void setup_server(server_t *serverp);
extern void delete_server(server_t *serverp);
extern boolean_t active_server(server_t *serverp);
extern boolean_t useless_server(server_t *serverp);

extern void delete_service(service_t *servicep);
extern service_t *lookup_service_by_name(bootstrap_info_t *bootstrap, name_t name);
extern service_t *lookup_service_by_port(mach_port_t port);
extern service_t *lookup_service_by_server(server_t *serverp);

extern bootstrap_info_t *lookup_bootstrap_by_port(mach_port_t port);
extern bootstrap_info_t *lookup_bootstrap_by_req_port(mach_port_t port);
extern void deactivate_bootstrap(bootstrap_info_t *bootstrap);
extern void deallocate_bootstrap(bootstrap_info_t *bootstrap);
extern boolean_t active_bootstrap(bootstrap_info_t *bootstrap);

extern void *ckmalloc(unsigned nbytes);

extern bootstrap_info_t bootstraps;		/* head of list of bootstrap ports */
extern server_t servers;		/* head of list of all servers */
extern service_t services;		/* head of list of all services */
extern unsigned nservices;		/* number of services in list */

#define	FIRST(q)		((q).next)
#define	NEXT(qe)		((qe)->next)
#define	PREV(qe)		((qe)->prev)
#define	IS_END(qe, q)		((qe) == &(q))
