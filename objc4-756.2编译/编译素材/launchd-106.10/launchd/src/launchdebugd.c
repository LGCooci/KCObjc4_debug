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
#include <sys/types.h>
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

#include "launch.h"

static void launch_print_obj(launch_data_t o, FILE *w);

static int kq = -1;

static void find_fds(launch_data_t o, const char *key __attribute__((unused)), void *context __attribute__((unused)))
{
	struct kevent kev;
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_FD:
		EV_SET(&kev, launch_data_get_fd(o), EVFILT_READ, EV_ADD, 0, 0, NULL);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1)
			syslog(LOG_DEBUG, "kevent(): %m");
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

int main(void)
{
	int r;
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	struct kevent kev;
	FILE *c;
	launch_data_t tmp, resp, msg = launch_data_alloc(LAUNCH_DATA_STRING);

	kq = kqueue();

	launch_data_set_string(msg, LAUNCH_KEY_CHECKIN);

	openlog(getprogname(), LOG_PERROR|LOG_PID|LOG_CONS, LOG_DAEMON);

	if ((resp = launch_msg(msg)) == NULL) {
		syslog(LOG_ERR, "launch_msg(\"" LAUNCH_KEY_CHECKIN "\"): %m");
		exit(EXIT_FAILURE);
	}

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_SOCKETS);
	if (tmp) {
		find_fds(tmp, NULL, NULL);
	} else {
		syslog(LOG_ERR, "No FDs found to answer requests on!");
		exit(EXIT_FAILURE);
	}

	launch_data_free(resp);

	if ((r = kevent(kq, NULL, 0, &kev, 1, NULL)) == -1) {
		syslog(LOG_ERR, "kevent(): %m");
		exit(EXIT_FAILURE);
	} else if (r == 0) {
		exit(EXIT_SUCCESS);
	}
	if ((r = accept(kev.ident, (struct sockaddr *)&ss, &slen)) == -1) {
		syslog(LOG_ERR, "accept(): %m");
		exit(EXIT_FAILURE);
	}

	c = fdopen(r, "r+");

	fprintf(c, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n");
		        
	fprintf(c, "<html>\n<body>\n");

	launch_data_set_string(msg, LAUNCH_KEY_GETJOBS);

	resp = launch_msg(msg);

	launch_print_obj(resp, c);

	fprintf(c, "</body>\n</html>\n");

	fclose(c);

	exit(EXIT_SUCCESS);
}

static void launch_print_obj_dict_callback(launch_data_t obj, const char *key, void *context)
{
	FILE *w = context;

	fprintf(w, "<i>%s</i>\n", key);
	if (launch_data_get_type(obj) != LAUNCH_DATA_ARRAY &&
			launch_data_get_type(obj) != LAUNCH_DATA_DICTIONARY)
		fprintf(w, "<ul><li>\n");
	launch_print_obj(obj, w);
	if (launch_data_get_type(obj) != LAUNCH_DATA_ARRAY &&
			launch_data_get_type(obj) != LAUNCH_DATA_DICTIONARY)
		fprintf(w, "</li></ul>\n");
}

static void launch_print_obj(launch_data_t o, FILE *w)
{
	size_t i;


        switch (launch_data_get_type(o)) {
        case LAUNCH_DATA_DICTIONARY:
		fprintf(w, "<ul><li>\n");
		launch_data_dict_iterate(o, launch_print_obj_dict_callback, w);
		fprintf(w, "</li></ul>\n");
                break;
        case LAUNCH_DATA_ARRAY:
		fprintf(w, "<ol>\n");
                for (i = 0; i < launch_data_array_get_count(o); i++) {
			fprintf(w, "<li>");
                        launch_print_obj(launch_data_array_get_index(o, i), w);
			fprintf(w, "</li>\n");
		}
		fprintf(w, "</ol>\n");
                break;
        case LAUNCH_DATA_INTEGER:
                fprintf(w, "Number: %lld", launch_data_get_integer(o));
                break;
        case LAUNCH_DATA_REAL:
                fprintf(w, "Float: %f", launch_data_get_real(o));
                break;
        case LAUNCH_DATA_STRING:
                fprintf(w, "String: %s", launch_data_get_string(o));
                break;
        case LAUNCH_DATA_OPAQUE:
                fprintf(w, "Opaque: %p size %zu", launch_data_get_opaque(o), launch_data_get_opaque_size(o));
                break;
        case LAUNCH_DATA_FD:
                fprintf(w, "FD: %d", launch_data_get_fd(o));
                break;
        case LAUNCH_DATA_BOOL:
                fprintf(w, "Bool: %s", launch_data_get_bool(o) ? "true" : "false");
                break;
        default:
                fprintf(w, "type %d is unknown", launch_data_get_type(o));
                break;
        }
}
