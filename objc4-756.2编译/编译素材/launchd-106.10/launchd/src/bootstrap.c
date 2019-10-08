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
 * bootstrap.c -- implementation of bootstrap main service loop
 */

/*
 * Imports
 */
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/boolean.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/mig_errors.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/bootstrap.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/exception.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <libc.h>
#include <paths.h>
#include <syslog.h>
#include <pwd.h>

#include <bsm/audit.h>
#include <bsm/libbsm.h>

#include "bootstrap.h"
#include "bootstrap_internal.h"
#include "lists.h"
#include "launchd.h"

/* Mig should produce a declaration for this,  but doesn't */
extern boolean_t bootstrap_server(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

auditinfo_t inherited_audit;
mach_port_t inherited_bootstrap_port = MACH_PORT_NULL;
bool forward_ok = false;
bool debugging = false;
bool register_self = false;
const char *register_name = NULL;
task_t	bootstrap_self = MACH_PORT_NULL;

static uid_t inherited_uid = 0;
static bool shutdown_in_progress = false;

#ifndef ASSERT
#define ASSERT(p)
#endif

/*
 * Private macros
 */
#define	NELEM(x)		(sizeof(x)/sizeof(x[0]))
#define	END_OF(x)		(&(x)[NELEM(x)])
#define	streq(a,b)		(strcmp(a,b) == 0)

/*
 * Private declarations
 */	
static void init_ports(void);
static void start_server(server_t *serverp);
static void exec_server(server_t *serverp);
static char **argvize(const char *string);
static void *demand_loop(void *arg);
void *mach_server_loop(void *);
extern kern_return_t bootstrap_register
(
	mach_port_t bootstrapport,
	name_t servicename,
	mach_port_t serviceport
);

/*
 * Private ports we hold receive rights for.  We also hold receive rights
 * for all the privileged ports.  Those are maintained in the server
 * structs.
 */
mach_port_t bootstrap_port_set;
mach_port_t demand_port_set;
pthread_t	demand_thread;

mach_port_t notify_port;
mach_port_t backup_port;


static mach_msg_return_t
inform_server_loop(
        mach_port_name_t about,
	mach_msg_option_t options)
{
	mach_port_destroyed_notification_t not;
	mach_msg_size_t size = sizeof(not) - sizeof(not.trailer);

	not.not_header.msgh_id = DEMAND_REQUEST;
	not.not_header.msgh_remote_port = backup_port;
	not.not_header.msgh_local_port = MACH_PORT_NULL;
	not.not_header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
	not.not_header.msgh_size = size;
	not.not_body.msgh_descriptor_count = 1;
	not.not_port.type = MACH_MSG_PORT_DESCRIPTOR;
	not.not_port.disposition = MACH_MSG_TYPE_PORT_NAME;
	not.not_port.name = about;
	return mach_msg(&not.not_header, MACH_SEND_MSG|options, size,
			0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static void
notify_server_loop(mach_port_name_t about)
{
	mach_msg_return_t result;

	result = inform_server_loop(about, MACH_MSG_OPTION_NONE);
	if (result != MACH_MSG_SUCCESS)
		syslog(LOG_ERR, "notify_server_loop: mach_msg(): %s", mach_error_string(result));
}

void mach_start_shutdown(__unused int signalnum)
{
	shutdown_in_progress = TRUE;
	(void) inform_server_loop(MACH_PORT_NULL, MACH_SEND_TIMEOUT);
}

mach_port_t mach_init_init(void)
{
	kern_return_t result;
	pthread_attr_t attr;

	bootstrap_self = mach_task_self();
	inherited_uid = getuid();
	getaudit(&inherited_audit);
	init_lists();
	init_ports();

	result = task_get_bootstrap_port(bootstrap_self, &inherited_bootstrap_port);
	if (result != KERN_SUCCESS) {
		syslog(LOG_ALERT, "task_get_bootstrap_port(): %s", mach_error_string(result));
		exit(EXIT_FAILURE);
	}
	if (inherited_bootstrap_port == MACH_PORT_NULL)
		forward_ok = FALSE;

	/* We set this explicitly as we start each child */
	task_set_bootstrap_port(bootstrap_self, MACH_PORT_NULL);

	/* register "self" port with anscestor */		
	if (register_self && forward_ok) {
		result = bootstrap_register(inherited_bootstrap_port,
							(char *)register_name,
							bootstraps.bootstrap_port);
		if (result != KERN_SUCCESS)
			panic("register self(): %s", mach_error_string(result));
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	result = pthread_create(&demand_thread, &attr, demand_loop, NULL);
	if (result)
		panic("pthread_create(): %s", strerror(result));
	pthread_attr_destroy(&attr);

	return bootstraps.bootstrap_port;
}

static void
init_ports(void)
{
	kern_return_t result;

	/*
	 *	This task will become the bootstrap task.
	 */
	/* Create port set that server loop listens to */
	result = mach_port_allocate(
						bootstrap_self,
						MACH_PORT_RIGHT_PORT_SET,
						&bootstrap_port_set);
	if (result != KERN_SUCCESS)
		panic("port_set_allocate(): %s", mach_error_string(result));

	/* Create demand port set that second thread listens to */
	result = mach_port_allocate(
						bootstrap_self,
						MACH_PORT_RIGHT_PORT_SET,
						&demand_port_set);
	if (result != KERN_SUCCESS)
		panic("port_set_allocate(): %s", mach_error_string(result));

	/* Create notify port and add to server port set */
	result = mach_port_allocate(
						bootstrap_self,
						MACH_PORT_RIGHT_RECEIVE,
						&notify_port);
	if (result != KERN_SUCCESS)
		panic("mach_port_allocate(): %s", mach_error_string(result));

	result = mach_port_move_member(
						bootstrap_self,
						notify_port,
						bootstrap_port_set);
	if (result != KERN_SUCCESS)
		panic("mach_port_move_member(): %s", mach_error_string(result));
	
	/* Create backup port and add to server port set */
	result = mach_port_allocate(
						bootstrap_self,
						MACH_PORT_RIGHT_RECEIVE,
						&backup_port);
	if (result != KERN_SUCCESS)
		panic("mach_port_allocate(): %s", mach_error_string(result));

	result = mach_port_move_member(
						bootstrap_self,
						backup_port,
						bootstrap_port_set);
	if (result != KERN_SUCCESS)
		panic("mach_port_move_member(): %s", mach_error_string(result));
	
	/* Create "self" port and add to server port set */
	result = mach_port_allocate(
						bootstrap_self,
						MACH_PORT_RIGHT_RECEIVE,
						&bootstraps.bootstrap_port);
	if (result != KERN_SUCCESS)
		panic("mach_port_allocate(): %s", mach_error_string(result));
	result = mach_port_insert_right(
						bootstrap_self,
						bootstraps.bootstrap_port,
						bootstraps.bootstrap_port,
						MACH_MSG_TYPE_MAKE_SEND);
	if (result != KERN_SUCCESS)
		panic("mach_port_insert_right(): %s", mach_error_string(result));

	/* keep the root bootstrap port "active" */
	bootstraps.requestor_port = bootstraps.bootstrap_port;

	result = mach_port_move_member(
						bootstrap_self,
						bootstraps.bootstrap_port,
						bootstrap_port_set);
	if (result != KERN_SUCCESS)
		panic("mach_port_move_member(): %s", mach_error_string(result));
}

boolean_t
active_bootstrap(bootstrap_info_t *bootstrap)
{
	return (bootstrap->requestor_port != MACH_PORT_NULL);
}

boolean_t
useless_server(server_t *serverp)
{
	return (	!active_bootstrap(serverp->bootstrap) || 
				!lookup_service_by_server(serverp) ||
				!serverp->activity);
}

boolean_t
active_server(server_t *serverp)
{
	return (	serverp->port ||
			serverp->task_port || serverp->active_services);
}

static void
reap_server(server_t *serverp)
{
	kern_return_t result;
	pid_t	presult;
	int		wstatus;

	/*
	 * Reap our children.
	 */
	presult = waitpid(serverp->pid, &wstatus, WNOHANG);
	switch (presult) {
	case -1:
		syslog(LOG_DEBUG, "waitpid: cmd = %s: %m", serverp->cmd);
		break;

	case 0:
	{
		/* process must have switched mach tasks */
		mach_port_t old_port;

		old_port = serverp->task_port;
		mach_port_deallocate(mach_task_self(), old_port);
		serverp->task_port = MACH_PORT_NULL;

		result = task_for_pid(	mach_task_self(),
					serverp->pid,
					&serverp->task_port);
		if (result != KERN_SUCCESS) {
			syslog(LOG_INFO, "race getting new server task port for pid[%d]: %s",
			     serverp->pid, mach_error_string(result));
			break;
		}

		/* Request dead name notification to tell when new task dies */
		result = mach_port_request_notification(
					mach_task_self(),
					serverp->task_port,
					MACH_NOTIFY_DEAD_NAME,
					0,
					notify_port,
					MACH_MSG_TYPE_MAKE_SEND_ONCE,
					&old_port);
		if (result != KERN_SUCCESS) {
			syslog(LOG_INFO, "race setting up notification for new server task port for pid[%d]: %s",
			     serverp->pid, mach_error_string(result));
			break;
		}
		return;
	}

	default:
		if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus)) {
			syslog(LOG_NOTICE, "Server %x in bootstrap %x uid %d: \"%s\"[%d]: exited with status: %d",
			       serverp->port, serverp->bootstrap->bootstrap_port,
			       serverp->uid, serverp->cmd, serverp->pid, WEXITSTATUS(wstatus));
		} else if (WIFSIGNALED(wstatus)) {
			syslog(LOG_NOTICE, "Server %x in bootstrap %x uid %d: \"%s\"[%d]: exited abnormally: %s",
			       serverp->port, serverp->bootstrap->bootstrap_port,
			       serverp->uid, serverp->cmd, serverp->pid, strsignal(WTERMSIG(wstatus)));
		}
		break;
	}
		

	serverp->pid = 0;

	/*
	 * Release the server task port reference, if we ever
	 * got it in the first place.
	 */
	if (serverp->task_port != MACH_PORT_NULL) {
		result = mach_port_deallocate(
					mach_task_self(),
					serverp->task_port);
		if (result != KERN_SUCCESS)
			syslog(LOG_ERR, "mach_port_deallocate(): %s", mach_error_string(result));
		serverp->task_port = MACH_PORT_NULL;
	}
}

static void
demand_server(server_t *serverp)
{
	service_t *servicep;
	kern_return_t result;

	/*
	 * For on-demand servers, make sure that the service ports are
	 * back in on-demand portset.  Active service ports should come
	 * back through a PORT_DESTROYED notification.  We only have to
	 * worry about the inactive ports that may have been previously
	 * pulled from the set but never checked-in by the server.
	 */

	for (  servicep = FIRST(services)
			   ; !IS_END(servicep, services)
			   ; servicep = NEXT(servicep))
	{
		if (serverp == servicep->server && !servicep->isActive) {
			result = mach_port_move_member(
							mach_task_self(),
							servicep->port,
							demand_port_set);
			if (result != KERN_SUCCESS)
				panic("mach_port_move_member(): %s", mach_error_string(result));
		}
	}
}

static
void dispatch_server(server_t *serverp)
{
	if (!active_server(serverp)) {
		if (useless_server(serverp) || shutdown_in_progress)
			delete_server(serverp);
		else if (serverp->servertype == RESTARTABLE)
			start_server(serverp);
		else if (serverp->servertype == DEMAND)
			demand_server(serverp);
	}
}

void
setup_server(server_t *serverp)
{
	kern_return_t result;
	mach_port_t old_port;
	
	/* Allocate privileged port for requests from service */
	result = mach_port_allocate(mach_task_self(),
						MACH_PORT_RIGHT_RECEIVE ,
						&serverp->port);
	syslog(LOG_INFO, "Allocating port %x for server %s", serverp->port, serverp->cmd);
	if (result != KERN_SUCCESS)	
		panic("port_allocate(): %s", mach_error_string(result));

	/* Request no-senders notification so we can tell when server dies */
	result = mach_port_request_notification(mach_task_self(),
						serverp->port,
						MACH_NOTIFY_NO_SENDERS,
						1,
						serverp->port,
						MACH_MSG_TYPE_MAKE_SEND_ONCE,
						&old_port);
	if (result != KERN_SUCCESS)
		panic("mach_port_request_notification(): %s", mach_error_string(result));

	/* Add privileged server port to bootstrap port set */
	result = mach_port_move_member(mach_task_self(),
						serverp->port,
						bootstrap_port_set);
	if (result != KERN_SUCCESS)
		panic("mach_port_move_member(): %s", mach_error_string(result));
}

pid_t
fork_with_bootstrap_port(mach_port_t p)
{
	static pthread_mutex_t forklock = PTHREAD_MUTEX_INITIALIZER;
	kern_return_t result;
	pid_t r;
	size_t i;

	pthread_mutex_lock(&forklock);

        sigprocmask(SIG_BLOCK, &blocked_signals, NULL);

        result = task_set_bootstrap_port(mach_task_self(), p);
	if (result != KERN_SUCCESS)
		panic("task_set_bootstrap_port(): %s", mach_error_string(result));

        if (launchd_bootstrap_port != p) {
		result = mach_port_deallocate(mach_task_self(), p);
		if (result != KERN_SUCCESS)
			panic("mach_port_deallocate(): %s", mach_error_string(result));
	}

	r = fork();

	if (r > 0) {
		/* Post Tiger:
		 *
		 * We should set the bootstrap back to MACH_PORT_NULL instead
		 * of launchd_bootstrap_port. This will expose rare latent race
		 * condition bugs, given that some programs assume that the PID
		 * 1's bootstrap port is constant. This function clearly
		 * demonstrates that is no longer true.
		 *
		 * Those programs should be calling bootstrap_parent(), and not
		 * task_for_pid(1) followed by a call to get the bootstrap port
		 * on the task.
		 */
		result = task_set_bootstrap_port(mach_task_self(), launchd_bootstrap_port);
		if (result != KERN_SUCCESS)
			panic("task_set_bootstrap_port(): %s", mach_error_string(result));
	} else {
		for (i = 0; i <= NSIG; i++) {
			if (sigismember(&blocked_signals, i))
				signal(i, SIG_DFL);
		}
	}

	sigprocmask(SIG_UNBLOCK, &blocked_signals, NULL);
	
	pthread_mutex_unlock(&forklock);

	return r;
}

static void
start_server(server_t *serverp)
{
	kern_return_t result;
	mach_port_t old_port;
	int pid;

	/*
	 * Do what's appropriate to get bootstrap port setup in server task
	 */
	switch (serverp->servertype) {

	case MACHINIT:
		break;

	case SERVER:
	case DEMAND:
	case RESTARTABLE:
	  if (!serverp->port)
	      setup_server(serverp);

	  serverp->activity = 0;

	  /* Insert a send right */
	  result = mach_port_insert_right(mach_task_self(),
						serverp->port,
						serverp->port,
						MACH_MSG_TYPE_MAKE_SEND);
	  if (result != KERN_SUCCESS)
	  	panic("mach_port_insert_right(): %s", mach_error_string(result));

		pid = fork_with_bootstrap_port(serverp->port);
		if (pid < 0) {
			syslog(LOG_WARNING, "fork(): %m");
		} else if (pid == 0) {	/* CHILD */
			exec_server(serverp);
			exit(EXIT_FAILURE);
		} else {		/* PARENT */
			syslog(LOG_INFO, "Launched server %x in bootstrap %x uid %d: \"%s\": [pid %d]",
			     serverp->port, serverp->bootstrap->bootstrap_port,
				 serverp->uid, serverp->cmd, pid);
			serverp->pid = pid;
			result = task_for_pid(
							mach_task_self(),
							pid,
							&serverp->task_port);
			if (result != KERN_SUCCESS) {
				syslog(LOG_ERR, "getting server task port(): %s", mach_error_string(result));
				reap_server(serverp);
				dispatch_server(serverp);
				break;
			}
				
			/* Request dead name notification to tell when task dies */
			result = mach_port_request_notification(
							mach_task_self(),
							serverp->task_port,
							MACH_NOTIFY_DEAD_NAME,
							0,
							notify_port,
							MACH_MSG_TYPE_MAKE_SEND_ONCE,
							&old_port);
			if (result != KERN_SUCCESS) {
				syslog(LOG_ERR, "mach_port_request_notification(): %s", mach_error_string(result));
				reap_server(serverp);
				dispatch_server(serverp);
			}
		}
		break;
	}
}

static void
exec_server(server_t *serverp)
{
	char **argv;
	sigset_t mask;

	/*
	 * Setup environment for server, someday this should be Mach stuff
	 * rather than Unix crud
	 */
	argv = argvize(serverp->cmd);
	closelog();

	/*
	 * Set up the audit state for the user (if necessesary).
	 */
	if (inherited_uid == 0 &&
	    (serverp->auinfo.ai_auid != inherited_uid ||
	     serverp->auinfo.ai_asid != inherited_audit.ai_asid)) {
		struct passwd *pwd = NULL;

		pwd = getpwuid(serverp->auinfo.ai_auid);
		if (pwd == NULL) {
			panic("Disabled server %x bootstrap %x: \"%s\": getpwuid(%d) failed",
				 serverp->port, serverp->bootstrap->bootstrap_port,
				 serverp->cmd, serverp->auinfo.ai_auid);

		} else if (au_user_mask(pwd->pw_name, &serverp->auinfo.ai_mask) != 0) {
			panic("Disabled server %x bootstrap %x: \"%s\": au_user_mask(%s) failed",
				 serverp->port, serverp->bootstrap->bootstrap_port,
				 serverp->cmd, pwd->pw_name);
		} else if (setaudit(&serverp->auinfo) != 0)
			panic("Disabled server %x bootstrap %x: \"%s\": setaudit()",
				 serverp->port, serverp->bootstrap->bootstrap_port,
				 serverp->cmd);
	}

	if (serverp->uid != inherited_uid) {
		struct passwd *pwd = getpwuid(serverp->uid);
		gid_t g;

		if (NULL == pwd) {
			panic("Disabled server %x bootstrap %x: \"%s\": getpwuid(%d) failed",
				 serverp->port, serverp->bootstrap->bootstrap_port,
				 serverp->cmd, serverp->uid);
		}

		g = pwd->pw_gid;

		if (-1 == setgroups(1, &g)) {
			panic("Disabled server %x bootstrap %x: \"%s\": setgroups(1, %d): %s",
					serverp->port, serverp->bootstrap->bootstrap_port,
					serverp->cmd, g, strerror(errno));
		}

		if (-1 == setgid(g)) {
			panic("Disabled server %x bootstrap %x: \"%s\": setgid(%d): %s",
					serverp->port, serverp->bootstrap->bootstrap_port,
					serverp->cmd, g, strerror(errno));
		}

		if (-1 == setuid(serverp->uid)) {
			panic("Disabled server %x bootstrap %x: \"%s\": setuid(%d): %s",
					 serverp->port, serverp->bootstrap->bootstrap_port,
					   serverp->cmd, serverp->uid, strerror(errno));
		}
	}


	if (setsid() < 0) {
	  	/*
		 * We can't keep this from happening, but we shouldn't start
		 * the server not as a process group leader.  So, just fake like
		 * there was real activity, and exit the child.  If needed,
		 * we'll re-launch it under another pid.
		 */
		serverp->activity = 1;
		panic("Temporary failure server %x bootstrap %x: \"%s\": setsid(): %s",
			   serverp->port, serverp->bootstrap->bootstrap_port,
			   serverp->cmd, strerror(errno));
	}

	sigemptyset(&mask);
	(void) sigprocmask(SIG_SETMASK, &mask, (sigset_t *)NULL);

	setpriority(PRIO_PROCESS, 0, 0);
	execv(argv[0], argv);
	panic("Disabled server %x bootstrap %x: \"%s\": exec(): %s",
			   serverp->port,
			   serverp->bootstrap->bootstrap_port,
			   serverp->cmd,
			   strerror(errno));
}	

static char **
argvize(const char *string)
{
	static char *argv[100], args[1000];
	const char *cp;
	char *argp, term;
	unsigned int nargs;
	
	/*
	 * Convert a command line into an argv for execv
	 */
	nargs = 0;
	argp = args;
	
	for (cp = string; *cp;) {
		while (isspace(*cp))
			cp++;
		term = (*cp == '"') ? *cp++ : '\0';
		if (nargs < NELEM(argv))
			argv[nargs++] = argp;
		while (*cp && (term ? *cp != term : !isspace(*cp))
		 && argp < END_OF(args)) {
			if (*cp == '\\')
				cp++;
			*argp++ = *cp;
			if (*cp)
				cp++;
		}
		*argp++ = '\0';
	}
	argv[nargs] = NULL;
	return argv;
}

static void *
demand_loop(void *arg __attribute__((unused)))
{
	mach_msg_empty_rcv_t dummy;
	kern_return_t dresult;


	for(;;) {
		mach_port_name_array_t members;
		mach_msg_type_number_t membersCnt;
		mach_port_status_t status;
		mach_msg_type_number_t statusCnt;
		unsigned int i;

		/*
		 * Receive indication of message on demand service
		 * ports without actually receiving the message (we'll
		 * let the actual server do that.
		 */
		dresult = mach_msg(
							&dummy.header,
							MACH_RCV_MSG|MACH_RCV_LARGE,
							0,
							0,
							demand_port_set,
							0,
							MACH_PORT_NULL);
		if (dresult != MACH_RCV_TOO_LARGE) {
			syslog(LOG_ERR, "demand_loop: mach_msg(): %s", mach_error_string(dresult));
			continue;
		}

		/*
		 * Some port(s) now have messages on them, find out
		 * which ones (there is no indication of which port
		 * triggered in the MACH_RCV_TOO_LARGE indication).
		 */
		dresult = mach_port_get_set_status(
							mach_task_self(),
							demand_port_set,
							&members,
							&membersCnt);
		if (dresult != KERN_SUCCESS) {
			syslog(LOG_ERR, "demand_loop: mach_port_get_set_status(): %s", mach_error_string(dresult));
			continue;
		}

		for (i = 0; i < membersCnt; i++) {
			statusCnt = MACH_PORT_RECEIVE_STATUS_COUNT;
			dresult = mach_port_get_attributes(
								mach_task_self(),
								members[i],
								MACH_PORT_RECEIVE_STATUS,
								(mach_port_info_t)&status,
								&statusCnt);
			if (dresult != KERN_SUCCESS) {
				syslog(LOG_ERR, "demand_loop: mach_port_get_attributes(): %s", mach_error_string(dresult));
				continue;
			}

			/*
			 * For each port with messages, take it out of the
			 * demand service portset, and inform the main thread
			 * that it might have to start the server responsible
			 * for it.
			 */
			if (status.mps_msgcount) {
				dresult = mach_port_move_member(
								mach_task_self(),
								members[i],
								MACH_PORT_NULL);
				if (dresult != KERN_SUCCESS) {
					syslog(LOG_ERR, "demand_loop: mach_port_move_member(): %s", mach_error_string(dresult));
					continue;
				}
				notify_server_loop(members[i]);
			}
		}

		dresult = vm_deallocate(
						mach_task_self(),
						(vm_address_t) members,
						(vm_size_t) membersCnt * sizeof(mach_port_name_t));
		if (dresult != KERN_SUCCESS) {
			syslog(LOG_ERR, "demand_loop: vm_deallocate(): %s", mach_error_string(dresult));
			continue;
		}
	}
	return NULL;
}
								
/*
 * server_demux -- processes requests off our service port
 * Also handles notifications
 */

static boolean_t
server_demux(
	mach_msg_header_t *Request,
	mach_msg_header_t *Reply)
{
    bootstrap_info_t *bootstrap;
    service_t *servicep;
    server_t *serverp;
    kern_return_t result;
	mig_reply_error_t *reply;
        
	syslog(LOG_DEBUG, "received message on port %x", Request->msgh_local_port);

	reply = (mig_reply_error_t *)Reply;

	/*
	 * Pick off notification messages
	 */
	if (Request->msgh_local_port == notify_port) {
		mach_port_name_t np;

		memset(reply, 0, sizeof(*reply));
		switch (Request->msgh_id) {
		case MACH_NOTIFY_DEAD_NAME:
			np = ((mach_dead_name_notification_t *)Request)->not_port;
			syslog(LOG_DEBUG, "Notified dead name %x", np);

			if (np == inherited_bootstrap_port) {
				inherited_bootstrap_port = MACH_PORT_NULL;
				forward_ok = FALSE;
			}
		
			/*
			 * Check to see if a subset requestor port was deleted.
			 */
			while ((bootstrap = lookup_bootstrap_by_req_port(np)) != NULL) {
				syslog(LOG_DEBUG, "Received dead name notification for bootstrap subset %x requestor port %x",
					 bootstrap->bootstrap_port, bootstrap->requestor_port);
				mach_port_deallocate(
									 mach_task_self(),
									 bootstrap->requestor_port);
				bootstrap->requestor_port = MACH_PORT_NULL;
				deactivate_bootstrap(bootstrap);
			}

			/*
			 * Check to see if a defined service has gone
			 * away.
			 */
			while ((servicep = lookup_service_by_port(np)) != NULL) {
				/*
				 * Port gone, registered service died.
				 */
				syslog(LOG_DEBUG, "Received dead name notification for service %s "
					  "on bootstrap port %x\n",
					  servicep->name, servicep->bootstrap);
				syslog(LOG_DEBUG, "Service %s failed - deallocate", servicep->name);
				delete_service(servicep);
			}

			/*
			 * Check to see if a launched server task has gone
			 * away.
			 */
			if ((serverp = lookup_server_by_task_port(np)) != NULL) {
				/*
				 * Port gone, server died or picked up new task.
				 */
				syslog(LOG_DEBUG, "Received task death notification for server %s ",
					  serverp->cmd);
				reap_server(serverp);
				dispatch_server(serverp);
			}

			mach_port_deallocate(mach_task_self(), np);
			reply->RetCode = KERN_SUCCESS;
			break;

		case MACH_NOTIFY_PORT_DELETED:
			np = ((mach_port_deleted_notification_t *)Request)->not_port;
			syslog(LOG_DEBUG, "port deleted notification on 0x%x", np);
			reply->RetCode = KERN_SUCCESS;
			break;

		case MACH_NOTIFY_SEND_ONCE:
			syslog(LOG_DEBUG, "notification send-once right went unused");
			reply->RetCode = KERN_SUCCESS;
			break;

		default:
			syslog(LOG_ERR, "Unexpected notification: %d", Request->msgh_id);
			reply->RetCode = KERN_FAILURE;
			break;
		}
	}

	else if (Request->msgh_local_port == backup_port) {
		mach_port_name_t np;

		memset(reply, 0, sizeof(*reply));

		np = ((mach_port_destroyed_notification_t *)Request)->not_port.name; 
		servicep = lookup_service_by_port(np);
		if (servicep != NULL) {
			serverp = servicep->server;

			switch (Request->msgh_id) {

			case MACH_NOTIFY_PORT_DESTROYED:
				/*
				 * Port sent back to us, server died.
				 */
				syslog(LOG_DEBUG, "Received destroyed notification for service %s",
					  servicep->name);
				syslog(LOG_DEBUG, "Service %x bootstrap %x backed up: %s",
				     servicep->port, servicep->bootstrap->bootstrap_port,
					 servicep->name);
				ASSERT(canReceive(servicep->port));
				servicep->isActive = FALSE;
				serverp->active_services--;
				dispatch_server(serverp);
				reply->RetCode = KERN_SUCCESS;
				break;

			case DEMAND_REQUEST:
				/* message reflected over from demand start thread */
				if (!active_server(serverp))
					start_server(serverp);
				reply->RetCode = KERN_SUCCESS;
				break;

			default:
				syslog(LOG_DEBUG, "Mysterious backup_port notification %d", Request->msgh_id);
				reply->RetCode = KERN_FAILURE;
				break;
			}
		} else {
			syslog(LOG_DEBUG, "Backup_port notification - previously deleted service");
			reply->RetCode = KERN_FAILURE;
		}
	}

	else if (Request->msgh_id == MACH_NOTIFY_NO_SENDERS) {
		mach_port_t ns = Request->msgh_local_port;

		if ((serverp = lookup_server_by_port(ns)) != NULL_SERVER) {
	  		/*
			 * A server we launched has released his bootstrap
			 * port send right.  We won't re-launch him unless
			 * his services came back to roost.  But we need to
			 * destroy the bootstrap port for fear of leaking.
			 */
			syslog(LOG_DEBUG, "server %s dropped server port", serverp->cmd);
			serverp->port = MACH_PORT_NULL;
			dispatch_server(serverp);
		} else if ((bootstrap = lookup_bootstrap_by_port(ns)) != NULL) {
			/*
			 * The last direct user of a deactivated bootstrap went away.
			 * We can finally free it.
			 */
			syslog(LOG_DEBUG, "Deallocating bootstrap %x: no more clients", ns);
			bootstrap->bootstrap_port = MACH_PORT_NULL;
			deallocate_bootstrap(bootstrap);
		}
		
		result = mach_port_mod_refs(
						mach_task_self(),
						ns,
						MACH_PORT_RIGHT_RECEIVE,
						-1);
		if (result != KERN_SUCCESS)
			panic("mach_port_mod_refs(): %s", mach_error_string(result));

		memset(reply, 0, sizeof(*reply));
		reply->RetCode = KERN_SUCCESS;
	}
     
	else {	/* must be a service request */
		syslog(LOG_DEBUG, "Handled request.");
		return bootstrap_server(Request, Reply);
	}
	return TRUE;
}

/*
 * server_loop -- pick requests off our service port and process them
 * Also handles notifications
 */
#define	bootstrapMaxRequestSize	1024
#define	bootstrapMaxReplySize	1024

void *
mach_server_loop(void *arg __attribute__((unused)))
{
	mach_msg_return_t mresult;

	for (;;) {
		mresult = mach_msg_server(
						server_demux,
						bootstrapMaxRequestSize,
						bootstrap_port_set,
                        MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_SENDER)|
                        MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0));
		if (mresult != MACH_MSG_SUCCESS)
				syslog(LOG_ERR, "mach_msg_server(): %s", mach_error_string(mresult));
	}
	return NULL;
}

bool
canReceive(mach_port_t port)
{
	mach_port_type_t p_type;
	kern_return_t result;
	
	result = mach_port_type(mach_task_self(), port, &p_type);
	if (result != KERN_SUCCESS) {
		syslog(LOG_ERR, "port_type(): %s", mach_error_string(result));
		return FALSE;
	}
	return ((p_type & MACH_PORT_TYPE_RECEIVE) != 0);
}


bool
canSend(mach_port_t port)
{
	mach_port_type_t p_type;
	kern_return_t result;
	
	result = mach_port_type(mach_task_self(), port, &p_type);
	if (result != KERN_SUCCESS) {
		syslog(LOG_ERR, "port_type(): %s", mach_error_string(result));
		return FALSE;
	}
	return ((p_type & MACH_PORT_TYPE_PORT_RIGHTS) != 0);
}
