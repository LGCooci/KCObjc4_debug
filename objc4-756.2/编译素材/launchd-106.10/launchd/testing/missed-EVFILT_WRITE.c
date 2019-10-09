/*
 * <rdar://problem/4038866> Lots of hangs in GetLaunchDaemonService state
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

static void do_parent(int thefd);
static void do_child(int thefd);

int main(void)
{
	int sp[2];

	if (-1 == socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) {
		fprintf(stderr, "socketpair(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	switch (fork()) {
	case -1:
		fprintf(stderr, "fork(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	case 0:
		close(sp[0]);
		do_child(sp[1]);
		break;
	default:
		close(sp[1]);
		do_parent(sp[0]);
		break;
	}

	exit(EXIT_SUCCESS);
}

static void do_child(int thefd)
{
	char buf[500000];
	fd_set rfds;
	int r, readhunks = 2;

	if (-1 == fcntl(thefd, F_SETFL, O_NONBLOCK)) {
		fprintf(stderr, "fcntl(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	for (;;) {
		if (2 == readhunks) {
			if (-1 == write(thefd, buf, 1)) {
				fprintf(stderr, "%d: write(): %s\n", __LINE__, strerror(errno));
				exit(EXIT_FAILURE);
			}
			readhunks = 0;
		}
		r = read(thefd, buf, sizeof(buf));

		if (-1 == r && errno != EAGAIN) {
			fprintf(stderr, "%d: read(): %s\n", __LINE__, strerror(errno));
			exit(EXIT_FAILURE);
		}

		if (r > 0)
			readhunks++;

		if (readhunks == 2)
			continue;

		FD_ZERO(&rfds);
		FD_SET(thefd, &rfds);

		select(thefd + 1, &rfds, NULL, NULL, NULL);
	}
}

static void do_parent(int thefd)
{
	struct timespec timeout = { 1, 0 };
	char buf[500000];
	struct kevent kev;
	int iter = 0, kq = kqueue();

	if (-1 == (kq = kqueue())) {
		fprintf(stderr, "kqueue(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (-1 == fcntl(thefd, F_SETFL, O_NONBLOCK)) {
		fprintf(stderr, "fcntl(): %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	EV_SET(&kev, thefd, EVFILT_READ, EV_ADD, 0, 0, NULL);

	if (-1 == kevent(kq, &kev, 1, NULL, 0, NULL)) {
		fprintf(stderr, "%d: kevent(): %s\n", __LINE__, strerror(errno));
		exit(EXIT_FAILURE);
	}

	for (;;) {
		switch (kevent(kq, NULL, 0, &kev, 1, &timeout)) {
		case -1:
			fprintf(stderr, "%d: kevent(): %s\n", __LINE__, strerror(errno));
			exit(EXIT_FAILURE);
		case 0:
			fprintf(stderr, "After %d iterations, 4038866 still exists!\n", iter);
			exit(EXIT_FAILURE);
		case 1:
			break;
		default:
			fprintf(stderr, "kevent should only return -1, 0 or 1 for this case!\n");
			exit(EXIT_FAILURE);
		}

		switch (kev.filter) {
		case EVFILT_READ:
			if (-1 == read(thefd, buf, sizeof(buf))) {
				fprintf(stderr, "read(): %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}
			if (-1 == write(thefd, buf, sizeof(buf))) {
				fprintf(stderr, "%d: write(): %s\n", __LINE__, strerror(errno));
				exit(EXIT_FAILURE);
			}
			EV_SET(&kev, thefd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
			if (-1 == kevent(kq, &kev, 1, NULL, 0, NULL)) {
				fprintf(stderr, "%d: kevent(): %s\n", __LINE__, strerror(errno));
				exit(EXIT_FAILURE);
			}
			break;
		case EVFILT_WRITE:
			if (-1 == write(thefd, buf, 1)) {
				fprintf(stderr, "%d: write(): %s\n", __LINE__, strerror(errno));
				exit(EXIT_FAILURE);
			}
			EV_SET(&kev, thefd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
			if (-1 == kevent(kq, &kev, 1, NULL, 0, NULL)) {
				fprintf(stderr, "%d: kevent(): %s\n", __LINE__, strerror(errno));
				exit(EXIT_FAILURE);
			}
			break;
		default:
			fprintf(stderr, "kevent filter isn't EVFILT_READ or EVFILT_WRITE!\n");
			exit(EXIT_FAILURE);
		}
		iter++;
	}
}
