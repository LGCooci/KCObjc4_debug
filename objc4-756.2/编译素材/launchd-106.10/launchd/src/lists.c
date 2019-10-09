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
 * lists.c -- implementation of list handling routines
 */

#include <mach/boolean.h>
#include <mach/mach_error.h>

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <bsm/audit.h>

#include "bootstrap_internal.h"
#include "lists.h"

/*
 * Exports
 */
bootstrap_info_t bootstraps;		/* head of list of all bootstrap ports */
server_t servers;		/* head of list of all servers */
service_t services;		/* head of list of all services */
unsigned nservices;		/* number of services in list */

#ifndef ASSERT
#define ASSERT(p)
#endif

/*
 * Private macros
 */
#define	NEW(type, num)	((type *)ckmalloc(sizeof(type) * num))
#define	STREQ(a, b)		(strcmp(a, b) == 0)
#define	NELEM(x)			(sizeof(x)/sizeof((x)[0]))
#define	LAST_ELEMENT(x)		((x)[NELEM(x)-1])

void
init_lists(void)
{
	bootstraps.ref_count = 2; /* make sure we never deallocate this one */
	bootstraps.next = bootstraps.prev = &bootstraps;
	bootstraps.parent = &bootstraps;
	servers.next = servers.prev = &servers;
	services.next = services.prev = &services;
	nservices = 0;
}

server_t *
new_server(
	bootstrap_info_t	*bootstrap,
	const char		*cmd,
	uid_t			uid,
	servertype_t		servertype,
	auditinfo_t		auinfo)
{
	server_t *serverp;

	syslog(LOG_DEBUG, "adding new server \"%s\" with uid %d", cmd, uid);	
	serverp = NEW(server_t, 1);
	if (serverp != NULL) {
		/* Doubly linked list */
		servers.prev->next = serverp;
		serverp->prev = servers.prev;
		serverp->next = &servers;
		servers.prev = serverp;

		bootstrap->ref_count++;
		serverp->bootstrap = bootstrap;

		serverp->pid = NO_PID;
		serverp->task_port = MACH_PORT_NULL;

		serverp->uid = uid;
		serverp->auinfo = auinfo;

		serverp->port = MACH_PORT_NULL;
		serverp->servertype = servertype;
		serverp->activity = 0;
		serverp->active_services = 0;
		strncpy(serverp->cmd, cmd, sizeof serverp->cmd);
		LAST_ELEMENT(serverp->cmd) = '\0';
	}
	return serverp;
}
	
service_t *
new_service(
	bootstrap_info_t	*bootstrap,
	const char	*name,
	mach_port_t		serviceport,
	boolean_t	isActive,
	servicetype_t	servicetype,
	server_t	*serverp)
{
        service_t *servicep;
        
	servicep = NEW(service_t, 1);
	if (servicep != NULL) {
		/* Doubly linked list */
		services.prev->next = servicep;
		servicep->prev = services.prev;
		servicep->next = &services;
		services.prev = servicep;
		
		nservices += 1;
		
		strncpy(servicep->name, name, sizeof servicep->name);
		LAST_ELEMENT(servicep->name) = '\0';
		servicep->servicetype = servicetype;
		servicep->bootstrap = bootstrap;
		servicep->port = serviceport;
		servicep->server = serverp;
		servicep->isActive = isActive;
	}
	return servicep;
}

bootstrap_info_t *
new_bootstrap(
	bootstrap_info_t	*parent,
	mach_port_t	bootstrapport,
	mach_port_t	requestorport)
{
	bootstrap_info_t *bootstrap;

	bootstrap = NEW(bootstrap_info_t, 1);
	if (bootstrap != NULL) {
		/* Doubly linked list */
		bootstraps.prev->next = bootstrap;
		bootstrap->prev = bootstraps.prev;
		bootstrap->next = &bootstraps;
		bootstraps.prev = bootstrap;
		
		bootstrap->bootstrap_port = bootstrapport;
		bootstrap->requestor_port = requestorport;

		bootstrap->ref_count = 1;
		bootstrap->parent = parent;
		parent->ref_count++;
	}
	return bootstrap;
}

bootstrap_info_t *
lookup_bootstrap_by_port(mach_port_t port)
{
	bootstrap_info_t *bootstrap;
	bootstrap_info_t *first;
	server_t *serverp;

	bootstrap = first = FIRST(bootstraps);
	do {  
		if (bootstrap->bootstrap_port == port)
			return bootstrap;
		bootstrap = NEXT(bootstrap);
	} while (bootstrap != first);
	
	for (  serverp = FIRST(servers)
	     ; !IS_END(serverp, servers)
	     ; serverp = NEXT(serverp))
	{
	  	if (port == serverp->port)
			return serverp->bootstrap;
	}
	return NULL;
}

bootstrap_info_t *
lookup_bootstrap_by_req_port(mach_port_t port)
{
	bootstrap_info_t *bootstrap;

	for (  bootstrap = FIRST(bootstraps)
	     ; !IS_END(bootstrap, bootstraps)
	     ; bootstrap = NEXT(bootstrap))
	{
		if (bootstrap->requestor_port == port)
			return bootstrap;
	}

	return NULL;
}

service_t *
lookup_service_by_name(bootstrap_info_t *bootstrap, name_t name)
{
	service_t *servicep;

	if (bootstrap)
		do {
			for (  servicep = FIRST(services)
			     ; !IS_END(servicep, services)
			     ; servicep = NEXT(servicep))
			{
				if (!STREQ(name, servicep->name))
					continue;
				if (bootstrap && servicep->bootstrap != bootstrap)
					continue;
				return servicep;
			}
		} while (bootstrap != &bootstraps &&
			(bootstrap = bootstrap->parent));
	return NULL;
}

void
unlink_service(service_t *servicep)
{
	ASSERT(servicep->prev->next == servicep);
	ASSERT(servicep->next->prev == servicep);
	servicep->prev->next = servicep->next;
	servicep->next->prev = servicep->prev;
	servicep->prev = servicep->next = servicep;	// idempotent
}

void
delete_service(service_t *servicep)
{
	unlink_service(servicep);
	switch (servicep->servicetype) {
	case REGISTERED:
		syslog(LOG_INFO, "Registered service %s deleted", servicep->name);
		mach_port_deallocate(mach_task_self(), servicep->port);
		break;
	case DECLARED:
		syslog(LOG_INFO, "Declared service %s now unavailable", servicep->name);
		mach_port_deallocate(mach_task_self(), servicep->port);
		mach_port_mod_refs(mach_task_self(), servicep->port,
				   MACH_PORT_RIGHT_RECEIVE, -1);
		break;
	default:
		syslog(LOG_ERR, "unknown service type %d", servicep->servicetype);
		break;
	}
	free(servicep);
	nservices -= 1;
}

void
delete_bootstrap_services(bootstrap_info_t *bootstrap)
{
	server_t  *serverp;
	service_t *servicep;
	service_t *next;
	
	for (  servicep = FIRST(services)
	     ; !IS_END(servicep, services)
	     ; servicep = next)
	{
		next = NEXT(servicep);
	  	if (bootstrap != servicep->bootstrap)
			continue;

		serverp = servicep->server;

		if (servicep->isActive && serverp)
			serverp->active_services--;

		delete_service(servicep);

		if (!serverp)
			continue;
		if (!active_server(serverp))
			delete_server(serverp);
	}
}

service_t *
lookup_service_by_port(mach_port_t port)
{
	service_t *servicep;
	
        for (  servicep = FIRST(services)
	     ; !IS_END(servicep, services)
	     ; servicep = NEXT(servicep))
	{
	  	if (port == servicep->port)
			return servicep;
	}
        return NULL;
}

service_t *
lookup_service_by_server(server_t *serverp)
{
	service_t *servicep;
	
        for (  servicep = FIRST(services)
	     ; !IS_END(servicep, services)
	     ; servicep = NEXT(servicep))
	{
	  	if (serverp == servicep->server)
			return servicep;
	}
        return NULL;
}

server_t *
lookup_server_by_task_port(mach_port_t port)
{
	server_t *serverp;
	
	for (  serverp = FIRST(servers)
	     ; !IS_END(serverp, servers)
	     ; serverp = NEXT(serverp))
	{
	  	if (port == serverp->task_port)
			return serverp;
	}
	return NULL;
}

server_t *
lookup_server_by_port(mach_port_t port)
{
	server_t *serverp;
	
	for (  serverp = FIRST(servers)
	     ; !IS_END(serverp, servers)
	     ; serverp = NEXT(serverp))
	{
	  	if (port == serverp->port)
			return serverp;
	}
	return NULL;
}

void
delete_server(server_t *serverp)
{
	service_t *servicep;
	service_t *next;

	syslog(LOG_INFO, "Deleting server %s", serverp->cmd);
	ASSERT(serverp->prev->next == serverp);
	ASSERT(serverp->next->prev == serverp);
	serverp->prev->next = serverp->next;
	serverp->next->prev = serverp->prev;

	for (  servicep = FIRST(services)
	     ; !IS_END(servicep, services)
	     ; servicep = next)
	{
		next = NEXT(servicep);
	  	if (serverp == servicep->server)
			delete_service(servicep);
	}

	deallocate_bootstrap(serverp->bootstrap);

	if (serverp->port)
		mach_port_mod_refs(mach_task_self(), serverp->port,
				   MACH_PORT_RIGHT_RECEIVE, -1);

	free(serverp);
}	

void
deactivate_bootstrap(bootstrap_info_t *bootstrap)
{
	bootstrap_info_t *deactivating_bootstraps;
	bootstrap_info_t *query_bootstrap;
	bootstrap_info_t *next_limit;
	bootstrap_info_t *limit;

	/*
	 * we need to recursively deactivate the whole subset tree below
	 * this point.  But we don't want to do real recursion because
	 * we don't have a limit on the depth.  So, build up a chain of
	 * active bootstraps anywhere underneath this one.
	 */
	deactivating_bootstraps = bootstrap;
	bootstrap->deactivate = NULL;
	for (next_limit = deactivating_bootstraps, limit = NULL
			 ; deactivating_bootstraps != limit
			 ; limit = next_limit, next_limit = deactivating_bootstraps)
	{
		for (bootstrap = deactivating_bootstraps
				 ; bootstrap != limit
				 ; bootstrap = bootstrap->deactivate)
		{
			for (  query_bootstrap = FIRST(bootstraps)
				   ; !IS_END(query_bootstrap, bootstraps)
				   ; query_bootstrap = NEXT(query_bootstrap))
			{
				if (query_bootstrap->parent == bootstrap &&
					query_bootstrap->requestor_port != MACH_PORT_NULL) {
					mach_port_deallocate(
										 mach_task_self(),
										 query_bootstrap->requestor_port);
					query_bootstrap->requestor_port = MACH_PORT_NULL;
					query_bootstrap->deactivate = deactivating_bootstraps;
					deactivating_bootstraps = query_bootstrap;
				}
			}
		}
	}

	/*
	 * The list is ordered with the furthest away progeny being
	 * at the front, and concluding with the one we started with.
	 * This allows us to safely deactivate and remove the reference
	 * each holds on their parent without fear of the chain getting
	 * corrupted (because each active parent holds a reference on
	 * itself and that doesn't get removed until we reach its spot
	 * in the list).
	 */
	do {
		bootstrap = deactivating_bootstraps;
		deactivating_bootstraps = bootstrap->deactivate;

		syslog(LOG_INFO, "deactivating bootstrap %x", bootstrap->bootstrap_port);

		delete_bootstrap_services(bootstrap);
		
		mach_port_deallocate(mach_task_self(), bootstrap->bootstrap_port);

		{
			mach_port_t previous;
			mach_port_request_notification(
					mach_task_self(),
					bootstrap->bootstrap_port,
					MACH_NOTIFY_NO_SENDERS,
					1,
					bootstrap->bootstrap_port,
					MACH_MSG_TYPE_MAKE_SEND_ONCE,
					&previous);
		}
	} while (deactivating_bootstraps != NULL);
}

void
deallocate_bootstrap(bootstrap_info_t *bootstrap)
{
	ASSERT(bootstrap->prev->next == bootstrap);
	ASSERT(bootstrap->next->prev == bootstrap);
	if (--bootstrap->ref_count > 0)
		return;

	bootstrap->prev->next = bootstrap->next;
	bootstrap->next->prev = bootstrap->prev;
	deallocate_bootstrap(bootstrap->parent);
	free(bootstrap);
}

void *
ckmalloc(unsigned nbytes)
{
	void *cp;
	
	if ((cp = malloc(nbytes)) == NULL) {
		syslog(LOG_EMERG, "malloc(): %m");
		exit(EXIT_FAILURE);
	}
	return cp;
}


