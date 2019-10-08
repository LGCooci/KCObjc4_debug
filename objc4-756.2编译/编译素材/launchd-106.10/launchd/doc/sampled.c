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

int main(void)
{
	struct timespec timeout = { 60, 0 };
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	struct kevent kev;
	launch_data_t tmp, resp, msg = launch_data_new_string(LAUNCH_KEY_CHECKIN);
	size_t i;
	int kq;

	openlog(getprogname(), LOG_PERROR|LOG_PID|LOG_CONS, LOG_DAEMON);

	if (-1 == (kq = kqueue())) {
		syslog(LOG_ERR, "kqueue(): %m");
		exit(EXIT_FAILURE);
	}

	if ((resp = launch_msg(msg)) == NULL) {
		syslog(LOG_ERR, "launch_msg(\"" LAUNCH_KEY_CHECKIN "\") IPC failure: %m");
		exit(EXIT_FAILURE);
	}

	if (LAUNCH_DATA_ERRNO == launch_data_get_type(resp)) {
		errno = launch_data_get_errno(resp);
		syslog(LOG_ERR, "Check-in failed: %m");
		exit(EXIT_FAILURE);
	}

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_TIMEOUT);
	if (tmp)
		timeout.tv_sec = launch_data_get_integer(tmp);

	tmp = launch_data_dict_lookup(resp, LAUNCH_JOBKEY_SOCKETS);
	if (NULL == tmp) {
		syslog(LOG_ERR, "No sockets found to answer requests on!");
		exit(EXIT_FAILURE);
	}

	if (launch_data_dict_get_count(tmp) > 1) {
		syslog(LOG_WARNING, "Some sockets will be ignored!");
	}

	tmp = launch_data_dict_lookup(tmp, "SampleListeners");
	if (NULL == tmp) {
		syslog(LOG_ERR, "No known sockets found to answer requests on!");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < launch_data_array_get_count(tmp); i++) {
		launch_data_t tmpi = launch_data_array_get_index(tmp, i);

		EV_SET(&kev, launch_data_get_fd(tmpi), EVFILT_READ, EV_ADD, 0, 0, NULL);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1) {
			syslog(LOG_DEBUG, "kevent(): %m");
			exit(EXIT_FAILURE);
		}
	}

	launch_data_free(msg);
	launch_data_free(resp);

	for (;;) {
		FILE *c;
		int r;

		if ((r = kevent(kq, NULL, 0, &kev, 1, &timeout)) == -1) {
			syslog(LOG_ERR, "kevent(): %m");
			exit(EXIT_FAILURE);
		} else if (r == 0) {
			exit(EXIT_SUCCESS);
		}

		if ((r = accept(kev.ident, (struct sockaddr *)&ss, &slen)) == -1) {
			syslog(LOG_ERR, "accept(): %m");
			continue; /* this isn't fatal */
		}

		c = fdopen(r, "r+");

		if (c) {
			fprintf(c, "hello world!\n");
			fclose(c);
		} else {
			close(r);
		}
	}
}
