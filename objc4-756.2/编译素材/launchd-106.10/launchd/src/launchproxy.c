/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <Security/AuthSession.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <libgen.h>
#include <getopt.h>
#include <signal.h>
#include <netdb.h>

#include "launch.h"

#if __GNUC__ >= 4
OSStatus SessionCreate(SessionCreationFlags flags, SessionAttributeBits attributes) __attribute__((weak));
#endif

static int kq = 0;

static void find_fds(launch_data_t o, const char *key __attribute__((unused)), void *context __attribute__((unused)))
{
        struct kevent kev;
        size_t i;
	int fd;

        switch (launch_data_get_type(o)) {
        case LAUNCH_DATA_FD:
		fd = launch_data_get_fd(o);
		if (-1 == fd)
			break;
		fcntl(fd, F_SETFD, 1);
		EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1)
			syslog(LOG_DEBUG, "kevent(%d): %m", fd);
                break;
        case LAUNCH_DATA_ARRAY:
                for (i = 0; i < launch_data_array_get_count(o); i++)
                        find_fds(launch_data_array_get_index(o, i), NULL, NULL);
                break;
        case LAUNCH_DATA_DICTIONARY:
                launch_data_dict_iterate(o, find_fds, NULL);
                break;
        default:
                break;
        }
}

int main(int argc __attribute__((unused)), char *argv[])
{
	struct timespec timeout = { 10, 0 };
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	struct kevent kev;
	int r, ec = EXIT_FAILURE;
	launch_data_t tmp, resp, msg = launch_data_alloc(LAUNCH_DATA_STRING);
	const char *prog = argv[1];
	bool w = false, dupstdout = true, dupstderr = true;

	launch_data_set_string(msg, LAUNCH_KEY_CHECKIN);

	openlog(getprogname(), LOG_PERROR|LOG_PID|LOG_CONS, LOG_LAUNCHD);

	kq = kqueue();

	if ((resp = launch_msg(msg)) == NULL) {
		syslog(LOG_ERR, "launch_msg(%s): %m", LAUNCH_KEY_CHECKIN);
		goto out;
	}

	launch_data_free(msg);

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_SOCKETS);
	if (tmp) {
		find_fds(tmp, NULL, NULL);
	} else {
		syslog(LOG_ERR, "No FDs found to answer requests on!");
		goto out;
	}

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_TIMEOUT);
	if (tmp)
		timeout.tv_sec = launch_data_get_integer(tmp);

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_PROGRAM);
	if (tmp)
		prog = launch_data_get_string(tmp);

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_INETDCOMPATIBILITY);
	if (tmp) {
		tmp = launch_data_dict_lookup(tmp, LAUNCH_JOBINETDCOMPATIBILITY_WAIT);
		if (tmp)
			w = launch_data_get_bool(tmp);
	}

	if (launch_data_dict_lookup(resp, LAUNCH_JOBKEY_STANDARDOUTPATH))
		dupstdout = false;

	if (launch_data_dict_lookup(resp, LAUNCH_JOBKEY_STANDARDERRORPATH))
		dupstderr = false;

	if (!w)
		signal(SIGCHLD, SIG_IGN);

	for (;;) {
		if ((r = kevent(kq, NULL, 0, &kev, 1, &timeout)) == -1) {
			syslog(LOG_DEBUG, "kevent(): %m");
			goto out;
		} else if (r == 0) {
			ec = EXIT_SUCCESS;
			goto out;
		}

		if (w) {
			dup2(kev.ident, STDIN_FILENO);
			if (dupstdout)
				dup2(kev.ident, STDOUT_FILENO);
			if (dupstderr)
				dup2(kev.ident, STDERR_FILENO);
			execv(prog, argv + 1);
			syslog(LOG_ERR, "execv(): %m");
			exit(EXIT_FAILURE);
		}

		if ((r = accept(kev.ident, (struct sockaddr *)&ss, &slen)) == -1) {
			if (errno == EWOULDBLOCK)
				continue;
			syslog(LOG_DEBUG, "accept(): %m");
			goto out;
		} else {
			char fromhost[NI_MAXHOST];
			char fromport[NI_MAXSERV];
			int gni_r;

			gni_r = getnameinfo((struct sockaddr *)&ss, slen,
					fromhost, sizeof(fromhost),
					fromport, sizeof(fromport),
					NI_NUMERICHOST | NI_NUMERICSERV);

			if (gni_r) {
				syslog(LOG_WARNING, "%s: getnameinfo(): %s", prog, gai_strerror(gni_r));
			} else {
				syslog(LOG_INFO, "%s: Connection from: %s on port: %s", prog, fromhost, fromport);
			}

			switch (fork()) {
			case -1:
				syslog(LOG_WARNING, "fork(): %m");
				goto out;
			case 0:
				break;
			default:
				close(r);
				continue;
			}

			if ((tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_SESSIONCREATE)) && launch_data_get_bool(tmp)) {
				if (SessionCreate) {
					OSStatus scr = SessionCreate(0, 0);
					if (scr != noErr)
						syslog(LOG_NOTICE, "%s: SessionCreate() failed: %d", prog, scr);
				} else {
					syslog(LOG_NOTICE, "%s: SessionCreate == NULL!", prog);
				}
			}
			fcntl(r, F_SETFL, 0);
			dup2(r, STDIN_FILENO);
			if (dupstdout)
				dup2(r, STDOUT_FILENO);
			if (dupstderr)
				dup2(r, STDERR_FILENO);
			signal(SIGCHLD, SIG_DFL);
			execv(prog, argv + 1);
			syslog(LOG_ERR, "execv(): %m");
			exit(EXIT_FAILURE);
		}
	}

out:
	exit(ec);
}
