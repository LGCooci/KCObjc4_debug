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
#ifdef EVFILT_MACH_IMPLEMENTED
#include <mach/mach_error.h>
#include <mach/port.h>
#endif
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/nd6.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <syslog.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>
#include <paths.h>
#include <pwd.h>
#include <grp.h>
#include <dlfcn.h>
#include <dirent.h>

#include "launch.h"
#include "launch_priv.h"
#include "launchd.h"

#include "bootstrap_internal.h"

#define LAUNCHD_MIN_JOB_RUN_TIME 10
#define LAUNCHD_REWARD_JOB_RUN_TIME 60
#define LAUNCHD_FAILED_EXITS_THRESHOLD 10
#define PID1LAUNCHD_CONF "/etc/launchd.conf"
#define LAUNCHD_CONF ".launchd.conf"
#define LAUNCHCTL_PATH "/bin/launchctl"
#define SECURITY_LIB "/System/Library/Frameworks/Security.framework/Versions/A/Security"
#define VOLFSDIR "/.vol"

extern char **environ;

struct jobcb {
	kq_callback kqjob_callback;
	TAILQ_ENTRY(jobcb) tqe;
	launch_data_t ldj;
	pid_t p;
	int execfd;
	time_t start_time;
	size_t failed_exits;
	int *vnodes;
	size_t vnodes_cnt;
	int *qdirs;
	size_t qdirs_cnt;
	unsigned int start_interval;
	struct tm *start_cal_interval;
	unsigned int checkedin:1, firstborn:1, debug:1, throttle:1, futureflags:28;
	char label[0];
};

struct conncb {
	kq_callback kqconn_callback;
	TAILQ_ENTRY(conncb) tqe;
	launch_t conn;
	struct jobcb *j;
	int disabled_batch:1, futureflags:31;
};

static TAILQ_HEAD(jobcbhead, jobcb) jobs = TAILQ_HEAD_INITIALIZER(jobs);
static TAILQ_HEAD(conncbhead, conncb) connections = TAILQ_HEAD_INITIALIZER(connections);
static int mainkq = 0;
static int asynckq = 0;
static int batch_disabler_count = 0;

static launch_data_t load_job(launch_data_t pload);
static launch_data_t get_jobs(const char *which);
static launch_data_t setstdio(int d, launch_data_t o);
static launch_data_t adjust_rlimits(launch_data_t in);
static void batch_job_enable(bool e, struct conncb *c);
static void do_shutdown(void);

static void listen_callback(void *, struct kevent *);
static void async_callback(void);
static void signal_callback(void *, struct kevent *);
static void fs_callback(void);
static void simple_zombie_reaper(void *, struct kevent *);
static void readcfg_callback(void *, struct kevent *);

static kq_callback kqlisten_callback = listen_callback;
static kq_callback kqasync_callback = (kq_callback)async_callback;
static kq_callback kqsignal_callback = signal_callback;
static kq_callback kqfs_callback = (kq_callback)fs_callback;
static kq_callback kqreadcfg_callback = readcfg_callback;
kq_callback kqsimple_zombie_reaper = simple_zombie_reaper;

static void job_watch(struct jobcb *j);
static void job_ignore(struct jobcb *j);
static void job_start(struct jobcb *j);
static void job_start_child(struct jobcb *j, int execfd);
static void job_setup_attributes(struct jobcb *j);
static void job_stop(struct jobcb *j);
static void job_reap(struct jobcb *j);
static void job_remove(struct jobcb *j);
static void job_set_alarm(struct jobcb *j);
static void job_callback(void *obj, struct kevent *kev);
static void job_log(struct jobcb *j, int pri, const char *msg, ...) __attribute__((format(printf, 3, 4)));
static void job_log_error(struct jobcb *j, int pri, const char *msg, ...) __attribute__((format(printf, 3, 4)));

static void ipc_open(int fd, struct jobcb *j);
static void ipc_close(struct conncb *c);
static void ipc_callback(void *, struct kevent *);
static void ipc_readmsg(launch_data_t msg, void *context);
static void ipc_readmsg2(launch_data_t data, const char *cmd, void *context);

#ifdef PID1_REAP_ADOPTED_CHILDREN
static void pid1waitpid(void);
static bool launchd_check_pid(pid_t p);
#endif
static void pid1_magic_init(bool sflag, bool vflag, bool xflag);
static void launchd_server_init(bool create_session);
static void conceive_firstborn(char *argv[]);

#ifdef EVFILT_MACH_IMPLEMENTED
static void *mach_demand_loop(void *);
static void mach_callback(void *, struct kevent *);
static kq_callback kqmach_callback = mach_callback;
#endif

static void usage(FILE *where);
static int _fd(int fd);

static void loopback_setup(void);
static void workaround3048875(int argc, char *argv[]);
static void reload_launchd_config(void);
static int dir_has_files(const char *path);
static void testfd_or_openfd(int fd, const char *path, int flags);
static void setup_job_env(launch_data_t obj, const char *key, void *context);
static void unsetup_job_env(launch_data_t obj, const char *key, void *context);


static size_t total_children = 0;
static pid_t readcfg_pid = 0;
static pid_t launchd_proper_pid = 0;
static bool launchd_inited = false;
static bool shutdown_in_progress = false;
static pthread_t mach_server_loop_thread;
mach_port_t launchd_bootstrap_port = MACH_PORT_NULL;
sigset_t blocked_signals = 0;
static char *pending_stdout = NULL;
static char *pending_stderr = NULL;

int main(int argc, char *argv[])
{
	static const int sigigns[] = { SIGHUP, SIGINT, SIGPIPE, SIGALRM,
		SIGTERM, SIGURG, SIGTSTP, SIGTSTP, SIGCONT, /*SIGCHLD,*/
		SIGTTIN, SIGTTOU, SIGIO, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF,
		SIGWINCH, SIGINFO, SIGUSR1, SIGUSR2 };
	struct kevent kev;
	size_t i;
	bool sflag = false, xflag = false, vflag = false, dflag = false;
	int ch;

	if (getpid() == 1)
		workaround3048875(argc, argv);

	setegid(getgid());
	seteuid(getuid());

	testfd_or_openfd(STDIN_FILENO, _PATH_DEVNULL, O_RDONLY);
	testfd_or_openfd(STDOUT_FILENO, _PATH_DEVNULL, O_WRONLY);
	testfd_or_openfd(STDERR_FILENO, _PATH_DEVNULL, O_WRONLY);

	openlog(getprogname(), LOG_CONS|(getpid() != 1 ? LOG_PID|LOG_PERROR : 0), LOG_LAUNCHD);
	setlogmask(LOG_UPTO(LOG_NOTICE));
	
	while ((ch = getopt(argc, argv, "dhsvx")) != -1) {
		switch (ch) {
		case 'd': dflag = true;   break;
		case 's': sflag = true;   break;
		case 'x': xflag = true;   break;
		case 'v': vflag = true;   break;
		case 'h': usage(stdout);  break;
		default:
			syslog(LOG_WARNING, "ignoring unknown arguments");
			usage(stderr);
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (dflag && daemon(0, 0) == -1)
		syslog(LOG_WARNING, "couldn't daemonize: %m");

	if ((mainkq = kqueue()) == -1) {
		syslog(LOG_EMERG, "kqueue(): %m");
		abort();
	}

	if ((asynckq = kqueue()) == -1) {
		syslog(LOG_ERR, "kqueue(): %m");
		abort();
	}
	
	if (kevent_mod(asynckq, EVFILT_READ, EV_ADD, 0, 0, &kqasync_callback) == -1) {
		syslog(LOG_ERR, "kevent_mod(asynckq, EVFILT_READ): %m");
		abort();
	}

	sigemptyset(&blocked_signals);

	for (i = 0; i < (sizeof(sigigns) / sizeof(int)); i++) {
		if (kevent_mod(sigigns[i], EVFILT_SIGNAL, EV_ADD, 0, 0, &kqsignal_callback) == -1)
			syslog(LOG_ERR, "failed to add kevent for signal: %d: %m", sigigns[i]);
		sigaddset(&blocked_signals, sigigns[i]);
		signal(sigigns[i], SIG_IGN);
	}

	/* sigh... ignoring SIGCHLD has side effects: we can't call wait*() */
	if (kevent_mod(SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, &kqsignal_callback) == -1)
		syslog(LOG_ERR, "failed to add kevent for signal: %d: %m", SIGCHLD);
	
	if (getpid() == 1) {
		pid1_magic_init(sflag, vflag, xflag);
	} else {
		launchd_bootstrap_port = bootstrap_port;
		launchd_server_init(argv[0] ? true : false);
	}

	/* do this after pid1_magic_init() to not catch ourselves mounting stuff */
	if (kevent_mod(0, EVFILT_FS, EV_ADD, 0, 0, &kqfs_callback) == -1)
		syslog(LOG_ERR, "kevent_mod(EVFILT_FS, &kqfs_callback): %m");


	if (argv[0])
		conceive_firstborn(argv);

	reload_launchd_config();

	if (argv[0])
		job_start(TAILQ_FIRST(&jobs));

	for (;;) {
		static struct timespec timeout = { 30, 0 };
		struct timespec *timeoutp = NULL;

		if (getpid() == 1) {
			if (readcfg_pid == 0)
				init_pre_kevent();
		} else {
			if (TAILQ_EMPTY(&jobs)) {
				/* launched on demand */
				timeoutp = &timeout;
			} else if (shutdown_in_progress && total_children == 0) {
				exit(EXIT_SUCCESS);
			}
		}

		switch (kevent(mainkq, NULL, 0, &kev, 1, timeoutp)) {
		case -1:
			syslog(LOG_DEBUG, "kevent(): %m");
			break;
		case 1:
			(*((kq_callback *)kev.udata))(kev.udata, &kev);
			break;
		case 0:
			if (timeoutp)
				exit(EXIT_SUCCESS);
			else
				syslog(LOG_DEBUG, "kevent(): spurious return with infinite timeout");
			break;
		default:
			syslog(LOG_DEBUG, "unexpected: kevent() returned something != 0, -1 or 1");
			break;
		}
	}
}

static void pid1_magic_init(bool sflag, bool vflag, bool xflag)
{
	pthread_attr_t attr;
	int memmib[2] = { CTL_HW, HW_PHYSMEM };
	int mvnmib[2] = { CTL_KERN, KERN_MAXVNODES };
	int hnmib[2] = { CTL_KERN, KERN_HOSTNAME };
	int tfp_r_mib[3] = { CTL_KERN, KERN_TFP, KERN_TFP_READ_GROUP };
	int tfp_rw_mib[3] = { CTL_KERN, KERN_TFP, KERN_TFP_RW_GROUP };
	gid_t tfp_r_gid = 0;
	gid_t tfp_rw_gid = 0;
	struct group *tfp_gr;
	uint64_t mem = 0;
	uint32_t mvn;
	size_t memsz = sizeof(mem);
	int pthr_r;

	if ((tfp_gr = getgrnam("procview"))) {
		tfp_r_gid = tfp_gr->gr_gid;
		sysctl(tfp_r_mib, 3, NULL, NULL, &tfp_r_gid, sizeof(tfp_r_gid));
	}

	if ((tfp_gr = getgrnam("procmod"))) {
		tfp_rw_gid = tfp_gr->gr_gid;
		sysctl(tfp_rw_mib, 3, NULL, NULL, &tfp_rw_gid, sizeof(tfp_rw_gid));
	}

	setpriority(PRIO_PROCESS, 0, -1);

	if (setsid() == -1)
		syslog(LOG_ERR, "setsid(): %m");

	if (chdir("/") == -1)
		syslog(LOG_ERR, "chdir(\"/\"): %m");

	if (sysctl(memmib, 2, &mem, &memsz, NULL, 0) == -1) {
		syslog(LOG_WARNING, "sysctl(\"%s\"): %m", "hw.physmem");
	} else {
		/* The following assignment of mem to itself if the size
		 * of data returned is 32 bits instead of 64 is a clever
		 * C trick to move the 32 bits on big endian systems to
		 * the least significant bytes of the 64 mem variable.
		 *
		 * On little endian systems, this is effectively a no-op.
		 */
		if (memsz == 4)
			mem = *(uint32_t *)&mem;
		mvn = mem / (64 * 1024) + 1024;
		if (sysctl(mvnmib, 2, NULL, NULL, &mvn, sizeof(mvn)) == -1)
			syslog(LOG_WARNING, "sysctl(\"%s\"): %m", "kern.maxvnodes");
	}
	if (sysctl(hnmib, 2, NULL, NULL, "localhost", sizeof("localhost")) == -1)
		syslog(LOG_WARNING, "sysctl(\"%s\"): %m", "kern.hostname");

	if (setlogin("root") == -1)
		syslog(LOG_ERR, "setlogin(\"root\"): %m");

	loopback_setup();

	if (mount("fdesc", "/dev", MNT_UNION, NULL) == -1)
		syslog(LOG_ERR, "mount(\"%s\", \"%s\", ...): %m", "fdesc", "/dev/");

	setenv("PATH", _PATH_STDPATH, 1);

	launchd_bootstrap_port = mach_init_init();
	task_set_bootstrap_port(mach_task_self(), launchd_bootstrap_port);
	bootstrap_port = MACH_PORT_NULL;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	pthr_r = pthread_create(&mach_server_loop_thread, &attr, mach_server_loop, NULL);
	if (pthr_r != 0) {
		syslog(LOG_ERR, "pthread_create(mach_server_loop): %s", strerror(pthr_r));
		exit(EXIT_FAILURE);
	}

	pthread_attr_destroy(&attr);

	init_boot(sflag, vflag, xflag);
}


#ifdef PID1_REAP_ADOPTED_CHILDREN
static bool launchd_check_pid(pid_t p)
{
	struct kevent kev;
	struct jobcb *j;

	TAILQ_FOREACH(j, &jobs, tqe) {
		if (j->p == p) {
			EV_SET(&kev, p, EVFILT_PROC, 0, 0, 0, j);
			j->kqjob_callback(j, &kev);
			return true;
		}
	}

	if (p == readcfg_pid) {
		readcfg_callback(NULL, NULL);
		return true;
	}

	return false;
}
#endif

static char *sockdir = NULL;
static char *sockpath = NULL;

static void launchd_clean_up(void)
{
	if (launchd_proper_pid != getpid())
		return;

	seteuid(0);
	setegid(0);

	if (-1 == unlink(sockpath))
		syslog(LOG_WARNING, "unlink(\"%s\"): %m", sockpath);
	else if (-1 == rmdir(sockdir))
		syslog(LOG_WARNING, "rmdir(\"%s\"): %m", sockdir);

	setegid(getgid());
	seteuid(getuid());
}

static void launchd_server_init(bool create_session)
{
	struct sockaddr_un sun;
	mode_t oldmask;
	int r, fd = -1, ourdirfd = -1;
	char ourdir[1024];

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;

	if (create_session) {
		snprintf(ourdir, sizeof(ourdir), "%s/%u.%u", LAUNCHD_SOCK_PREFIX, getuid(), getpid());
		snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/%u.%u/sock", LAUNCHD_SOCK_PREFIX, getuid(), getpid());
		setenv(LAUNCHD_SOCKET_ENV, sun.sun_path, 1);
	} else {
		snprintf(ourdir, sizeof(ourdir), "%s/%u", LAUNCHD_SOCK_PREFIX, getuid());
		snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/%u/sock", LAUNCHD_SOCK_PREFIX, getuid());
	}

	seteuid(0);
	setegid(0);

	if (mkdir(LAUNCHD_SOCK_PREFIX, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) == -1) {
		if (errno == EROFS) {
			goto out_bad;
		} else if (errno == EEXIST) {
			struct stat sb;
			stat(LAUNCHD_SOCK_PREFIX, &sb);
			if (!S_ISDIR(sb.st_mode)) {
				errno = EEXIST;
				syslog(LOG_ERR, "mkdir(\"%s\"): %m", LAUNCHD_SOCK_PREFIX);
				goto out_bad;
			}
		} else {
			syslog(LOG_ERR, "mkdir(\"%s\"): %m", LAUNCHD_SOCK_PREFIX);
			goto out_bad;
		}
	}

	unlink(ourdir);
	if (mkdir(ourdir, S_IRWXU) == -1) {
		if (errno == EROFS) {
			goto out_bad;
		} else if (errno == EEXIST) {
			struct stat sb;
			stat(ourdir, &sb);
			if (!S_ISDIR(sb.st_mode)) {
				errno = EEXIST;
				syslog(LOG_ERR, "mkdir(\"%s\"): %m", LAUNCHD_SOCK_PREFIX);
				goto out_bad;
			}
		} else {
			syslog(LOG_ERR, "mkdir(\"%s\"): %m", ourdir);
			goto out_bad;
		}
	}

	if (chown(ourdir, getuid(), getgid()) == -1)
		syslog(LOG_WARNING, "chown(\"%s\"): %m", ourdir);

	setegid(getgid());
	seteuid(getuid());

	ourdirfd = _fd(open(ourdir, O_RDONLY));
	if (ourdirfd == -1) {
		syslog(LOG_ERR, "open(\"%s\"): %m", ourdir);
		goto out_bad;
	}

	if (flock(ourdirfd, LOCK_EX|LOCK_NB) == -1) {
		if (errno == EWOULDBLOCK) {
			exit(EXIT_SUCCESS);
		} else {
			syslog(LOG_ERR, "flock(\"%s\"): %m", ourdir);
			goto out_bad;
		}
	}

	if (unlink(sun.sun_path) == -1 && errno != ENOENT) {
		if (errno != EROFS)
			syslog(LOG_ERR, "unlink(\"thesocket\"): %m");
		goto out_bad;
	}
	if ((fd = _fd(socket(AF_UNIX, SOCK_STREAM, 0))) == -1) {
		syslog(LOG_ERR, "socket(\"thesocket\"): %m");
		goto out_bad;
	}
	oldmask = umask(077);
	r = bind(fd, (struct sockaddr *)&sun, sizeof(sun));
	umask(oldmask);
	if (r == -1) {
		if (errno != EROFS)
			syslog(LOG_ERR, "bind(\"thesocket\"): %m");
		goto out_bad;
	}

	if (listen(fd, SOMAXCONN) == -1) {
		syslog(LOG_ERR, "listen(\"thesocket\"): %m");
		goto out_bad;
	}

	if (kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, &kqlisten_callback) == -1) {
		syslog(LOG_ERR, "kevent_mod(\"thesocket\", EVFILT_READ): %m");
		goto out_bad;
	}

	launchd_inited = true;

	sockdir = strdup(ourdir);
	sockpath = strdup(sun.sun_path);

	launchd_proper_pid = getpid();
	atexit(launchd_clean_up);

out_bad:
	setegid(getgid());
	seteuid(getuid());

	if (!launchd_inited) {
		if (fd != -1)
			close(fd);
		if (ourdirfd != -1)
			close(ourdirfd);
	}
}

static long long job_get_integer(launch_data_t j, const char *key)
{
	launch_data_t t = launch_data_dict_lookup(j, key);
	if (t)
		return launch_data_get_integer(t);
	else
		return 0;
}

static const char *job_get_string(launch_data_t j, const char *key)
{
	launch_data_t t = launch_data_dict_lookup(j, key);
	if (t)
		return launch_data_get_string(t);
	else
		return NULL;
}

static const char *job_get_file2exec(launch_data_t j)
{
	launch_data_t tmpi, tmp = launch_data_dict_lookup(j, LAUNCH_JOBKEY_PROGRAM);

	if (tmp) {
		return launch_data_get_string(tmp);
	} else {
		tmp = launch_data_dict_lookup(j, LAUNCH_JOBKEY_PROGRAMARGUMENTS);
		if (tmp) {
			tmpi = launch_data_array_get_index(tmp, 0);
			if (tmpi)
				return launch_data_get_string(tmpi);
		}
		return NULL;
	}
}

static bool job_get_bool(launch_data_t j, const char *key)
{
	launch_data_t t = launch_data_dict_lookup(j, key);
	if (t)
		return launch_data_get_bool(t);
	else
		return false;
}

static void ipc_open(int fd, struct jobcb *j)
{
	struct conncb *c = calloc(1, sizeof(struct conncb));

	fcntl(fd, F_SETFL, O_NONBLOCK);

	c->kqconn_callback = ipc_callback;
	c->conn = launchd_fdopen(fd);
	c->j = j;
	TAILQ_INSERT_TAIL(&connections, c, tqe);
	kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, &c->kqconn_callback);
}

static void simple_zombie_reaper(void *obj __attribute__((unused)), struct kevent *kev)
{
	waitpid(kev->ident, NULL, 0);
}

static void listen_callback(void *obj __attribute__((unused)), struct kevent *kev)
{
	struct sockaddr_un sun;
	socklen_t sl = sizeof(sun);
	int cfd;

	if ((cfd = _fd(accept(kev->ident, (struct sockaddr *)&sun, &sl))) == -1) {
		return;
	}

	ipc_open(cfd, NULL);
}

static void ipc_callback(void *obj, struct kevent *kev)
{
	struct conncb *c = obj;
	int r;
	
	if (kev->filter == EVFILT_READ) {
		if (launchd_msg_recv(c->conn, ipc_readmsg, c) == -1 && errno != EAGAIN) {
			if (errno != ECONNRESET)
				syslog(LOG_DEBUG, "%s(): recv: %m", __func__);
			ipc_close(c);
		}
	} else if (kev->filter == EVFILT_WRITE) {
		r = launchd_msg_send(c->conn, NULL);
		if (r == -1) {
			if (errno != EAGAIN) {
				syslog(LOG_DEBUG, "%s(): send: %m", __func__);
				ipc_close(c);
			}
		} else if (r == 0) {
			kevent_mod(launchd_getfd(c->conn), EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
		}
	} else {
		syslog(LOG_DEBUG, "%s(): unknown filter type!", __func__);
		ipc_close(c);
	}
}

static void set_user_env(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	setenv(key, launch_data_get_string(obj), 1);
}

static void launch_data_close_fds(launch_data_t o)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, (void (*)(launch_data_t, const char *, void *))launch_data_close_fds, NULL);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			launch_data_close_fds(launch_data_array_get_index(o, i));
		break;
	case LAUNCH_DATA_FD:
		if (launch_data_get_fd(o) != -1)
			close(launch_data_get_fd(o));
		break;
	default:
		break;
	}
}

static void launch_data_revoke_fds(launch_data_t o)
{
	size_t i;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, (void (*)(launch_data_t, const char *, void *))launch_data_revoke_fds, NULL);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			launch_data_revoke_fds(launch_data_array_get_index(o, i));
		break;
	case LAUNCH_DATA_FD:
		launch_data_set_fd(o, -1);
		break;
	default:
		break;
	}
}

static void job_ignore_fds(launch_data_t o, const char *key __attribute__((unused)), void *cookie)
{
	struct jobcb *j = cookie;
	size_t i;
	int fd;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, job_ignore_fds, cookie);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			job_ignore_fds(launch_data_array_get_index(o, i), NULL, cookie);
		break;
	case LAUNCH_DATA_FD:
		fd = launch_data_get_fd(o);
		if (-1 != fd) {
			job_log(j, LOG_DEBUG, "Ignoring FD: %d", fd);
			kevent_mod(fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		}
		break;
	default:
		break;
	}
}

static void job_ignore(struct jobcb *j)
{
	launch_data_t j_sockets = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_SOCKETS);
	size_t i;

	if (j_sockets)
		job_ignore_fds(j_sockets, NULL, j);

	for (i = 0; i < j->vnodes_cnt; i++) {
		kevent_mod(j->vnodes[i], EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
	}
	for (i = 0; i < j->qdirs_cnt; i++) {
		kevent_mod(j->qdirs[i], EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
	}
}

static void job_watch_fds(launch_data_t o, const char *key __attribute__((unused)), void *cookie)
{
	struct jobcb *j = cookie;
	size_t i;
	int fd;

	switch (launch_data_get_type(o)) {
	case LAUNCH_DATA_DICTIONARY:
		launch_data_dict_iterate(o, job_watch_fds, cookie);
		break;
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < launch_data_array_get_count(o); i++)
			job_watch_fds(launch_data_array_get_index(o, i), NULL, cookie);
		break;
	case LAUNCH_DATA_FD:
		fd = launch_data_get_fd(o);
		if (-1 != fd) {
			job_log(j, LOG_DEBUG, "Watching FD: %d", fd);
			kevent_mod(fd, EVFILT_READ, EV_ADD, 0, 0, cookie);
		}
		break;
	default:
		break;
	}
}

static void job_watch(struct jobcb *j)
{
	launch_data_t ld_qdirs = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_QUEUEDIRECTORIES);
	launch_data_t ld_vnodes = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_WATCHPATHS);
	launch_data_t j_sockets = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_SOCKETS);
	size_t i;

	if (j_sockets)
		job_watch_fds(j_sockets, NULL, &j->kqjob_callback);

	for (i = 0; i < j->vnodes_cnt; i++) {
		if (-1 == j->vnodes[i]) {
			launch_data_t ld_idx = launch_data_array_get_index(ld_vnodes, i);
			const char *thepath = launch_data_get_string(ld_idx);

			if (-1 == (j->vnodes[i] = _fd(open(thepath, O_EVTONLY))))
				job_log_error(j, LOG_ERR, "open(\"%s\", O_EVTONLY)", thepath);
		}
		kevent_mod(j->vnodes[i], EVFILT_VNODE, EV_ADD|EV_CLEAR,
				NOTE_WRITE|NOTE_EXTEND|NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE|NOTE_ATTRIB|NOTE_LINK,
				0, &j->kqjob_callback);
	}

	for (i = 0; i < j->qdirs_cnt; i++) {
		kevent_mod(j->qdirs[i], EVFILT_VNODE, EV_ADD|EV_CLEAR,
				NOTE_WRITE|NOTE_EXTEND|NOTE_ATTRIB|NOTE_LINK, 0, &j->kqjob_callback);
	}

	for (i = 0; i < j->qdirs_cnt; i++) {
		launch_data_t ld_idx = launch_data_array_get_index(ld_qdirs, i);
		const char *thepath = launch_data_get_string(ld_idx);
		int dcc_r;

		if (-1 == (dcc_r = dir_has_files(thepath))) {
			job_log_error(j, LOG_ERR, "dir_has_files(\"%s\", ...)", thepath);
		} else if (dcc_r > 0 && !shutdown_in_progress) {
			job_start(j);
			break;
		}
	}
}

static void job_stop(struct jobcb *j)
{
	if (j->p)
		kill(j->p, SIGTERM);
}

static void job_remove(struct jobcb *j)
{
	launch_data_t tmp;
	size_t i;

	job_log(j, LOG_DEBUG, "Removed");

	TAILQ_REMOVE(&jobs, j, tqe);
	if (j->p) {
		if (kevent_mod(j->p, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &kqsimple_zombie_reaper) == -1) {
			job_reap(j);
		} else {
			job_stop(j);
		}
	}
	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_USERENVIRONMENTVARIABLES)))
		launch_data_dict_iterate(tmp, unsetup_job_env, NULL);
	launch_data_close_fds(j->ldj);
	launch_data_free(j->ldj);
	if (j->execfd)
		close(j->execfd);
	for (i = 0; i < j->vnodes_cnt; i++)
		if (-1 != j->vnodes[i])
			close(j->vnodes[i]);
	if (j->vnodes)
		free(j->vnodes);
	for (i = 0; i < j->qdirs_cnt; i++)
		if (-1 != j->qdirs[i])
			close(j->qdirs[i]);
	if (j->qdirs)
		free(j->qdirs);
	if (j->start_interval)
		kevent_mod((uintptr_t)&j->start_interval, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	if (j->start_cal_interval) {
		kevent_mod((uintptr_t)j->start_cal_interval, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
		free(j->start_cal_interval);
	}
	kevent_mod((uintptr_t)j, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	free(j);
}

struct readmsg_context {
	struct conncb *c;
	launch_data_t resp;
};

static void ipc_readmsg(launch_data_t msg, void *context)
{
	struct readmsg_context rmc = { context, NULL };

	if (LAUNCH_DATA_DICTIONARY == launch_data_get_type(msg)) {
		launch_data_dict_iterate(msg, ipc_readmsg2, &rmc);
	} else if (LAUNCH_DATA_STRING == launch_data_get_type(msg)) {
		ipc_readmsg2(NULL, launch_data_get_string(msg), &rmc);
	} else {
		rmc.resp = launch_data_new_errno(EINVAL);
	}

	if (NULL == rmc.resp)
		rmc.resp = launch_data_new_errno(ENOSYS);

	launch_data_close_fds(msg);

	if (launchd_msg_send(rmc.c->conn, rmc.resp) == -1) {
		if (errno == EAGAIN) {
			kevent_mod(launchd_getfd(rmc.c->conn), EVFILT_WRITE, EV_ADD, 0, 0, &rmc.c->kqconn_callback);
		} else {
			syslog(LOG_DEBUG, "launchd_msg_send() == -1: %m");
			ipc_close(rmc.c);
		}
	}
	launch_data_free(rmc.resp);
}


static void ipc_readmsg2(launch_data_t data, const char *cmd, void *context)
{
	struct readmsg_context *rmc = context;
	launch_data_t resp = NULL;
	struct jobcb *j;

	if (rmc->resp)
		return;

	if (!strcmp(cmd, LAUNCH_KEY_STARTJOB)) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(j->label, launch_data_get_string(data))) {
				job_start(j);
				resp = launch_data_new_errno(0);
			}
		}
		if (NULL == resp)
			resp = launch_data_new_errno(ESRCH);
	} else if (!strcmp(cmd, LAUNCH_KEY_STOPJOB)) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(j->label, launch_data_get_string(data))) {
				job_stop(j);
				resp = launch_data_new_errno(0);
			}
		}
		if (NULL == resp)
			resp = launch_data_new_errno(ESRCH);
	} else if (!strcmp(cmd, LAUNCH_KEY_REMOVEJOB)) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(j->label, launch_data_get_string(data))) {
				job_remove(j);
				resp = launch_data_new_errno(0);
			}
		}
		if (NULL == resp)
			resp = launch_data_new_errno(ESRCH);
	} else if (!strcmp(cmd, LAUNCH_KEY_SUBMITJOB)) {
		if (launch_data_get_type(data) == LAUNCH_DATA_ARRAY) {
			launch_data_t tmp;
			size_t i;

			resp = launch_data_alloc(LAUNCH_DATA_ARRAY);
			for (i = 0; i < launch_data_array_get_count(data); i++) {
				tmp = load_job(launch_data_array_get_index(data, i));
				launch_data_array_set_index(resp, tmp, i);
			}
		} else {
			resp = load_job(data);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_UNSETUSERENVIRONMENT)) {
		unsetenv(launch_data_get_string(data));
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETUSERENVIRONMENT)) {
		char **tmpenviron = environ;
		resp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
		for (; *tmpenviron; tmpenviron++) {
			char envkey[1024];
			launch_data_t s = launch_data_alloc(LAUNCH_DATA_STRING);
			launch_data_set_string(s, strchr(*tmpenviron, '=') + 1);
			strncpy(envkey, *tmpenviron, sizeof(envkey));
			*(strchr(envkey, '=')) = '\0';
			launch_data_dict_insert(resp, s, envkey);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_SETUSERENVIRONMENT)) {
		launch_data_dict_iterate(data, set_user_env, NULL);
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_CHECKIN)) {
		if (rmc->c->j) {
			resp = launch_data_copy(rmc->c->j->ldj);
			if (NULL == launch_data_dict_lookup(resp, LAUNCH_JOBKEY_TIMEOUT)) {
				launch_data_t to = launch_data_new_integer(LAUNCHD_MIN_JOB_RUN_TIME);
				launch_data_dict_insert(resp, to, LAUNCH_JOBKEY_TIMEOUT);
			}
			rmc->c->j->checkedin = true;
		} else {
			resp = launch_data_new_errno(EACCES);
		}
	} else if (!strcmp(cmd, LAUNCH_KEY_RELOADTTYS)) {
		update_ttys();
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_SHUTDOWN)) {
		do_shutdown();
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETJOBS)) {
		resp = get_jobs(NULL);
		launch_data_revoke_fds(resp);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETRESOURCELIMITS)) {
		resp = adjust_rlimits(NULL);
	} else if (!strcmp(cmd, LAUNCH_KEY_SETRESOURCELIMITS)) {
		resp = adjust_rlimits(data);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETJOB)) {
		resp = get_jobs(launch_data_get_string(data));
		launch_data_revoke_fds(resp);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETJOBWITHHANDLES)) {
		resp = get_jobs(launch_data_get_string(data));
	} else if (!strcmp(cmd, LAUNCH_KEY_SETLOGMASK)) {
		resp = launch_data_new_integer(setlogmask(launch_data_get_integer(data)));
	} else if (!strcmp(cmd, LAUNCH_KEY_GETLOGMASK)) {
		int oldmask = setlogmask(LOG_UPTO(LOG_DEBUG));
		resp = launch_data_new_integer(oldmask);
		setlogmask(oldmask);
	} else if (!strcmp(cmd, LAUNCH_KEY_SETUMASK)) {
		resp = launch_data_new_integer(umask(launch_data_get_integer(data)));
	} else if (!strcmp(cmd, LAUNCH_KEY_GETUMASK)) {
		mode_t oldmask = umask(0);
		resp = launch_data_new_integer(oldmask);
		umask(oldmask);
	} else if (!strcmp(cmd, LAUNCH_KEY_GETRUSAGESELF)) {
		struct rusage rusage;
		getrusage(RUSAGE_SELF, &rusage);
		resp = launch_data_new_opaque(&rusage, sizeof(rusage));
	} else if (!strcmp(cmd, LAUNCH_KEY_GETRUSAGECHILDREN)) {
		struct rusage rusage;
		getrusage(RUSAGE_CHILDREN, &rusage);
		resp = launch_data_new_opaque(&rusage, sizeof(rusage));
	} else if (!strcmp(cmd, LAUNCH_KEY_SETSTDOUT)) {
		resp = setstdio(STDOUT_FILENO, data);
	} else if (!strcmp(cmd, LAUNCH_KEY_SETSTDERR)) {
		resp = setstdio(STDERR_FILENO, data);
	} else if (!strcmp(cmd, LAUNCH_KEY_BATCHCONTROL)) {
		batch_job_enable(launch_data_get_bool(data), rmc->c);
		resp = launch_data_new_errno(0);
	} else if (!strcmp(cmd, LAUNCH_KEY_BATCHQUERY)) {
		resp = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(resp, batch_disabler_count == 0);
	}

	rmc->resp = resp;
}

static launch_data_t setstdio(int d, launch_data_t o)
{
	launch_data_t resp = launch_data_new_errno(0);

	if (launch_data_get_type(o) == LAUNCH_DATA_STRING) {
		char **where = &pending_stderr;
		if (d == STDOUT_FILENO)
			where = &pending_stdout;
		if (*where)
			free(*where);
		*where = strdup(launch_data_get_string(o));
	} else if (launch_data_get_type(o) == LAUNCH_DATA_FD) {
		dup2(launch_data_get_fd(o), d);
	} else {
		launch_data_set_errno(resp, EINVAL);
	}

	return resp;
}

static void batch_job_enable(bool e, struct conncb *c)
{
	if (e && c->disabled_batch) {
		batch_disabler_count--;
		c->disabled_batch = 0;
		if (batch_disabler_count == 0)
			kevent_mod(asynckq, EVFILT_READ, EV_ENABLE, 0, 0, &kqasync_callback);
	} else if (!e && !c->disabled_batch) {
		if (batch_disabler_count == 0)
			kevent_mod(asynckq, EVFILT_READ, EV_DISABLE, 0, 0, &kqasync_callback);
		batch_disabler_count++;
		c->disabled_batch = 1;
	}
}

static launch_data_t load_job(launch_data_t pload)
{
	launch_data_t tmp, resp;
	const char *label;
	struct jobcb *j;
	bool startnow, hasprog = false, hasprogargs = false;

	if ((label = job_get_string(pload, LAUNCH_JOBKEY_LABEL))) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(j->label, label)) {
				resp = launch_data_new_errno(EEXIST);
				goto out;
			}
		}
	} else {
		resp = launch_data_new_errno(EINVAL);
		goto out;
	}

	if (launch_data_dict_lookup(pload, LAUNCH_JOBKEY_PROGRAM))
		hasprog = true;
	if (launch_data_dict_lookup(pload, LAUNCH_JOBKEY_PROGRAMARGUMENTS))
		hasprogargs = true;

	if (!hasprog && !hasprogargs) {
		resp = launch_data_new_errno(EINVAL);
		goto out;
	}

	j = calloc(1, sizeof(struct jobcb) + strlen(label) + 1);
	strcpy(j->label, label);
	j->ldj = launch_data_copy(pload);
	launch_data_revoke_fds(pload);
	j->kqjob_callback = job_callback;


	if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_ONDEMAND) == NULL) {
		tmp = launch_data_alloc(LAUNCH_DATA_BOOL);
		launch_data_set_bool(tmp, true);
		launch_data_dict_insert(j->ldj, tmp, LAUNCH_JOBKEY_ONDEMAND);
	}

	TAILQ_INSERT_TAIL(&jobs, j, tqe);

	j->debug = job_get_bool(j->ldj, LAUNCH_JOBKEY_DEBUG);

	startnow = !job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND);

	if (job_get_bool(j->ldj, LAUNCH_JOBKEY_RUNATLOAD))
		startnow = true;

	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_QUEUEDIRECTORIES))) {
		size_t i;

		j->qdirs_cnt = launch_data_array_get_count(tmp);
		j->qdirs = malloc(sizeof(int) * j->qdirs_cnt);

		for (i = 0; i < j->qdirs_cnt; i++) {
			const char *thepath = launch_data_get_string(launch_data_array_get_index(tmp, i));

			if (-1 == (j->qdirs[i] = _fd(open(thepath, O_EVTONLY))))
				job_log_error(j, LOG_ERR, "open(\"%s\", O_EVTONLY)", thepath);
		}

	}

	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_STARTINTERVAL))) {
		j->start_interval = launch_data_get_integer(tmp);

		if (j->start_interval == 0)
			job_log(j, LOG_WARNING, "StartInterval is zero, ignoring");
		else if (-1 == kevent_mod((uintptr_t)&j->start_interval, EVFILT_TIMER, EV_ADD, NOTE_SECONDS, j->start_interval, &j->kqjob_callback))
			job_log_error(j, LOG_ERR, "adding kevent timer");
	}

	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_STARTCALENDARINTERVAL))) {
		launch_data_t tmp_k;

		j->start_cal_interval = calloc(1, sizeof(struct tm));
		j->start_cal_interval->tm_min = -1;
		j->start_cal_interval->tm_hour = -1;
		j->start_cal_interval->tm_mday = -1;
		j->start_cal_interval->tm_wday = -1;
		j->start_cal_interval->tm_mon = -1;

		if (LAUNCH_DATA_DICTIONARY == launch_data_get_type(tmp)) {
			if ((tmp_k = launch_data_dict_lookup(tmp, LAUNCH_JOBKEY_CAL_MINUTE)))
				j->start_cal_interval->tm_min = launch_data_get_integer(tmp_k);
			if ((tmp_k = launch_data_dict_lookup(tmp, LAUNCH_JOBKEY_CAL_HOUR)))
				j->start_cal_interval->tm_hour = launch_data_get_integer(tmp_k);
			if ((tmp_k = launch_data_dict_lookup(tmp, LAUNCH_JOBKEY_CAL_DAY)))
				j->start_cal_interval->tm_mday = launch_data_get_integer(tmp_k);
			if ((tmp_k = launch_data_dict_lookup(tmp, LAUNCH_JOBKEY_CAL_WEEKDAY)))
				j->start_cal_interval->tm_wday = launch_data_get_integer(tmp_k);
			if ((tmp_k = launch_data_dict_lookup(tmp, LAUNCH_JOBKEY_CAL_MONTH)))
				j->start_cal_interval->tm_mon = launch_data_get_integer(tmp_k);
		}

		job_set_alarm(j);
	}
	
	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_WATCHPATHS))) {
		size_t i;

		j->vnodes_cnt = launch_data_array_get_count(tmp);
		j->vnodes = malloc(sizeof(int) * j->vnodes_cnt);

		for (i = 0; i < j->vnodes_cnt; i++) {
			const char *thepath = launch_data_get_string(launch_data_array_get_index(tmp, i));

			if (-1 == (j->vnodes[i] = _fd(open(thepath, O_EVTONLY))))
				job_log_error(j, LOG_ERR, "open(\"%s\", O_EVTONLY)", thepath);
		}

	}

	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_USERENVIRONMENTVARIABLES)))
		launch_data_dict_iterate(tmp, setup_job_env, NULL);
	
	if (job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND))
		job_watch(j);

	if (startnow)
		job_start(j);

	resp = launch_data_new_errno(0);
out:
	return resp;
}

static launch_data_t get_jobs(const char *which)
{
	struct jobcb *j;
	launch_data_t tmp, resp = NULL;

	if (which) {
		TAILQ_FOREACH(j, &jobs, tqe) {
			if (!strcmp(which, j->label))
				resp = launch_data_copy(j->ldj);
		}
		if (resp == NULL)
			resp = launch_data_new_errno(ESRCH);
	} else {
		resp = launch_data_alloc(LAUNCH_DATA_DICTIONARY);

		TAILQ_FOREACH(j, &jobs, tqe) {
			tmp = launch_data_copy(j->ldj);
			launch_data_dict_insert(resp, tmp, j->label);
		}
	}

	return resp;
}

static void usage(FILE *where)
{
	fprintf(where, "%s: [-d] [-- command [args ...]]\n", getprogname());
	fprintf(where, "\t-d\tdaemonize\n");
	fprintf(where, "\t-h\tthis usage statement\n");

	if (where == stdout)
		exit(EXIT_SUCCESS);
}

#ifdef EVFILT_MACH_IMPLEMENTED
static void **machcbtable = NULL;
static size_t machcbtable_cnt = 0;
static int machcbreadfd = -1;
static int machcbwritefd = -1;
static mach_port_t mach_demand_port_set = MACH_PORT_NULL;
static pthread_t mach_demand_thread;

static void mach_callback(void *obj __attribute__((unused)), struct kevent *kev __attribute__((unused)))
{
	struct kevent mkev;
	mach_port_t mp;

	read(machcbreadfd, &mp, sizeof(mp));

	EV_SET(&mkev, mp, EVFILT_MACHPORT, 0, 0, 0, machcbtable[MACH_PORT_INDEX(mp)]);

	(*((kq_callback *)mkev.udata))(mkev.udata, &mkev);
}
#endif

int kevent_mod(uintptr_t ident, short filter, u_short flags, u_int fflags, intptr_t data, void *udata)
{
	struct kevent kev;
	int q = mainkq;
#ifdef EVFILT_MACH_IMPLEMENTED
	kern_return_t kr;
	pthread_attr_t attr;
	int pthr_r, pfds[2];
#endif

	if (EVFILT_TIMER == filter || EVFILT_VNODE == filter)
		q = asynckq;

	if (flags & EV_ADD && NULL == udata) {
		syslog(LOG_ERR, "%s(): kev.udata == NULL!!!", __func__);
		syslog(LOG_ERR, "kev: ident %d filter %d flags 0x%x fflags 0x%x",
				ident, filter, flags, fflags);
		errno = EINVAL;
		return -1;
	}

#ifdef EVFILT_MACH_IMPLEMENTED
	if (filter != EVFILT_MACHPORT) {
#endif
#ifdef PID1_REAP_ADOPTED_CHILDREN
		if (filter == EVFILT_PROC && getpid() == 1)
			return 0;
#endif
		EV_SET(&kev, ident, filter, flags, fflags, data, udata);
		return kevent(q, &kev, 1, NULL, 0, NULL);
#ifdef EVFILT_MACH_IMPLEMENTED
	}

	if (machcbtable == NULL) {
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		pthr_r = pthread_create(&mach_demand_thread, &attr, mach_demand_loop, NULL);
		if (pthr_r != 0) {
			syslog(LOG_ERR, "pthread_create(mach_demand_loop): %s", strerror(pthr_r));
			exit(EXIT_FAILURE);
		}

		pthread_attr_destroy(&attr);

		machcbtable = malloc(0);
		pipe(pfds);
		machcbwritefd = _fd(pfds[1]);
		machcbreadfd = _fd(pfds[0]);
		kevent_mod(machcbreadfd, EVFILT_READ, EV_ADD, 0, 0, &kqmach_callback);
		kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &mach_demand_port_set);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_ERR, "mach_port_allocate(demand_port_set): %s", mach_error_string(kr));
			exit(EXIT_FAILURE);
		}
	}

	if (flags & EV_ADD) {
		kr = mach_port_move_member(mach_task_self(), ident, mach_demand_port_set);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_ERR, "mach_port_move_member(): %s", mach_error_string(kr));
			exit(EXIT_FAILURE);
		}

		if (MACH_PORT_INDEX(ident) > machcbtable_cnt)
			machcbtable = realloc(machcbtable, MACH_PORT_INDEX(ident) * sizeof(void *));

		machcbtable[MACH_PORT_INDEX(ident)] = udata;
	} else if (flags & EV_DELETE) {
		kr = mach_port_move_member(mach_task_self(), ident, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_ERR, "mach_port_move_member(): %s", mach_error_string(kr));
			exit(EXIT_FAILURE);
		}
	} else {
		syslog(LOG_DEBUG, "kevent_mod(EVFILT_MACHPORT) with flags: %d", flags);
		errno = EINVAL;
		return -1;
	}

	return 0;
#endif
}

static int _fd(int fd)
{
	if (fd >= 0)
		fcntl(fd, F_SETFD, 1);
	return fd;
}

static void ipc_close(struct conncb *c)
{
	batch_job_enable(true, c);

	TAILQ_REMOVE(&connections, c, tqe);
	launchd_close(c->conn);
	free(c);
}

static void setup_job_env(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	if (LAUNCH_DATA_STRING == launch_data_get_type(obj))
		setenv(key, launch_data_get_string(obj), 1);
}

static void unsetup_job_env(launch_data_t obj, const char *key, void *context __attribute__((unused)))
{
	if (LAUNCH_DATA_STRING == launch_data_get_type(obj))
		unsetenv(key);
}

static void job_reap(struct jobcb *j)
{
	bool od = job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND);
	time_t td = time(NULL) - j->start_time;
	bool bad_exit = false;
	int status;

	job_log(j, LOG_DEBUG, "Reaping");

	if (j->execfd) {
		close(j->execfd);
		j->execfd = 0;
	}

#ifdef PID1_REAP_ADOPTED_CHILDREN
	if (getpid() == 1)
		status = pid1_child_exit_status;
	else
#endif
	if (-1 == waitpid(j->p, &status, 0)) {
		job_log_error(j, LOG_ERR, "waitpid(%d, ...)", j->p);
		return;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		job_log(j, LOG_WARNING, "exited with exit code: %d", WEXITSTATUS(status));
		bad_exit = true;
	}

	if (WIFSIGNALED(status)) {
		int s = WTERMSIG(status);
		if (SIGKILL == s || SIGTERM == s) {
			job_log(j, LOG_NOTICE, "exited: %s", strsignal(s));
		} else {
			job_log(j, LOG_WARNING, "exited abnormally: %s", strsignal(s));
			bad_exit = true;
		}
	}

	if (!od) {
		if (td < LAUNCHD_MIN_JOB_RUN_TIME) {
			job_log(j, LOG_WARNING, "respawning too quickly! throttling");
			bad_exit = true;
			j->throttle = true;
		} else if (td >= LAUNCHD_REWARD_JOB_RUN_TIME) {
			job_log(j, LOG_INFO, "lived long enough, forgiving past exit failures");
			j->failed_exits = 0;
		}
	}

	if (bad_exit)
		j->failed_exits++;

	if (j->failed_exits > 0) {
		int failures_left = LAUNCHD_FAILED_EXITS_THRESHOLD - j->failed_exits;
		if (failures_left)
			job_log(j, LOG_WARNING, "%d more failure%s without living at least %d seconds will cause job removal",
					failures_left, failures_left > 1 ? "s" : "", LAUNCHD_REWARD_JOB_RUN_TIME);
	}

	total_children--;
	j->p = 0;
}

static bool job_restart_fitness_test(struct jobcb *j)
{
	bool od = job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND);

	if (j->firstborn) {
		job_log(j, LOG_DEBUG, "first born died, begin shutdown");
		do_shutdown();
		return false;
	} else if (job_get_bool(j->ldj, LAUNCH_JOBKEY_SERVICEIPC) && !j->checkedin) {
		job_log(j, LOG_WARNING, "failed to checkin");
		job_remove(j);
		return false;
	} else if (j->failed_exits >= LAUNCHD_FAILED_EXITS_THRESHOLD) {
		job_log(j, LOG_WARNING, "too many failures in succession");
		job_remove(j);
		return false;
	} else if (od || shutdown_in_progress) {
		if (!od && shutdown_in_progress)
			job_log(j, LOG_NOTICE, "exited while shutdown is in progress, will not restart unless demand requires it");
		job_watch(j);
		return false;
	}

	return true;
}

static void job_callback(void *obj, struct kevent *kev)
{
	struct jobcb *j = obj;
	bool d = j->debug;
	bool startnow = true;
	int oldmask = 0;

	if (d) {
		oldmask = setlogmask(LOG_UPTO(LOG_DEBUG));
		job_log(j, LOG_DEBUG, "log level debug temporarily enabled while processing job");
	}

	if (kev->filter == EVFILT_PROC) {
		job_reap(j);

		startnow = job_restart_fitness_test(j);

		if (startnow && j->throttle) {
			j->throttle = false;
			job_log(j, LOG_WARNING, "will restart in %d seconds", LAUNCHD_MIN_JOB_RUN_TIME);
			if (-1 == kevent_mod((uintptr_t)j, EVFILT_TIMER, EV_ADD|EV_ONESHOT,
						NOTE_SECONDS, LAUNCHD_MIN_JOB_RUN_TIME, &j->kqjob_callback)) {
				job_log_error(j, LOG_WARNING, "failed to setup timer callback!, starting now!");
			} else {
				startnow = false;
			}
		}
	} else if (kev->filter == EVFILT_TIMER && (void *)kev->ident == j->start_cal_interval) {
		job_set_alarm(j);
	} else if (kev->filter == EVFILT_VNODE) {
		size_t i;
		const char *thepath = NULL;

		for (i = 0; i < j->vnodes_cnt; i++) {
			if (j->vnodes[i] == (int)kev->ident) {
				launch_data_t ld_vnodes = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_WATCHPATHS);

				thepath = launch_data_get_string(launch_data_array_get_index(ld_vnodes, i));

				job_log(j, LOG_DEBUG, "watch path modified: %s", thepath);

				if ((NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE) & kev->fflags) {
					job_log(j, LOG_DEBUG, "watch path invalidated: %s", thepath);
					close(j->vnodes[i]);
					j->vnodes[i] = -1; /* this will get fixed in job_watch() */
				}
			}
		}

		for (i = 0; i < j->qdirs_cnt; i++) {
			if (j->qdirs[i] == (int)kev->ident) {
				launch_data_t ld_qdirs = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_QUEUEDIRECTORIES);
				int dcc_r;

				thepath = launch_data_get_string(launch_data_array_get_index(ld_qdirs, i));

				job_log(j, LOG_DEBUG, "queue directory modified: %s", thepath);

				if (-1 == (dcc_r = dir_has_files(thepath))) {
					job_log_error(j, LOG_ERR, "dir_has_files(\"%s\", ...)", thepath);
				} else if (0 == dcc_r) {
					job_log(j, LOG_DEBUG, "spurious wake up, directory empty: %s", thepath);
					startnow = false;
				}
			}
		}
		/* if we get here, then the vnodes either wasn't a qdir, or if it was, it has entries in it */
	} else if (kev->filter == EVFILT_READ && (int)kev->ident == j->execfd) {
		if (kev->data > 0) {
			int e;

			read(j->execfd, &e, sizeof(e));
			errno = e;
			job_log_error(j, LOG_ERR, "execve()");
			job_remove(j);
			j = NULL;
			startnow = false;
		} else {
			close(j->execfd);
			j->execfd = 0;
		}
		startnow = false;
	}

	if (startnow)
		job_start(j);

	if (d) {
		/* the job might have been removed, must not call job_log() */
		syslog(LOG_DEBUG, "restoring original log mask");
		setlogmask(oldmask);
	}
}

static void job_start(struct jobcb *j)
{
	int spair[2];
	int execspair[2];
	bool sipc;
	char nbuf[64];
	pid_t c;

	job_log(j, LOG_DEBUG, "Starting");

	if (j->p) {
		job_log(j, LOG_DEBUG, "already running");
		return;
	}

	j->checkedin = false;

	sipc = job_get_bool(j->ldj, LAUNCH_JOBKEY_SERVICEIPC);

	if (job_get_bool(j->ldj, LAUNCH_JOBKEY_INETDCOMPATIBILITY))
		sipc = true;

	if (sipc)
		socketpair(AF_UNIX, SOCK_STREAM, 0, spair);

	socketpair(AF_UNIX, SOCK_STREAM, 0, execspair);

	time(&j->start_time);

	switch (c = fork_with_bootstrap_port(launchd_bootstrap_port)) {
	case -1:
		job_log_error(j, LOG_ERR, "fork() failed, will try again in one second");
		close(execspair[0]);
		close(execspair[1]);
		if (sipc) {
			close(spair[0]);
			close(spair[1]);
		}
		if (job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND))
			job_ignore(j);
		break;
	case 0:
		close(execspair[0]);
		/* wait for our parent to say they've attached a kevent to us */
		read(_fd(execspair[1]), &c, sizeof(c));
		if (j->firstborn) {
			setpgid(getpid(), getpid());
			if (isatty(STDIN_FILENO)) {
				if (tcsetpgrp(STDIN_FILENO, getpid()) == -1)
					job_log_error(j, LOG_WARNING, "tcsetpgrp()");
			}
		}

		if (sipc) {
			close(spair[0]);
			sprintf(nbuf, "%d", spair[1]);
			setenv(LAUNCHD_TRUSTED_FD_ENV, nbuf, 1);
		}
		job_start_child(j, execspair[1]);
		break;
	default:
		close(execspair[1]);
		j->execfd = _fd(execspair[0]);
		if (sipc) {
			close(spair[1]);
			ipc_open(_fd(spair[0]), j);
		}
		if (kevent_mod(j->execfd, EVFILT_READ, EV_ADD, 0, 0, &j->kqjob_callback) == -1)
			job_log_error(j, LOG_ERR, "kevent_mod(j->execfd): %m");
		if (kevent_mod(c, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &j->kqjob_callback) == -1) {
			job_log_error(j, LOG_ERR, "kevent()");
			job_reap(j);
		} else {
			j->p = c;
			total_children++;
			if (job_get_bool(j->ldj, LAUNCH_JOBKEY_ONDEMAND))
				job_ignore(j);
		}
		/* this unblocks the child and avoids a race
		 * between the above fork() and the kevent_mod() */
		write(j->execfd, &c, sizeof(c));
		break;
	}
}

static void job_start_child(struct jobcb *j, int execfd)
{
	launch_data_t ldpa = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_PROGRAMARGUMENTS);
	bool inetcompat = job_get_bool(j->ldj, LAUNCH_JOBKEY_INETDCOMPATIBILITY);
	size_t i, argv_cnt;
	const char **argv, *file2exec = "/usr/libexec/launchproxy";
	int r;
	bool hasprog = false;

	job_setup_attributes(j);

	if (ldpa) {
		argv_cnt = launch_data_array_get_count(ldpa);
		argv = alloca((argv_cnt + 2) * sizeof(char *));
		for (i = 0; i < argv_cnt; i++)
			argv[i + 1] = launch_data_get_string(launch_data_array_get_index(ldpa, i));
		argv[argv_cnt + 1] = NULL;
	} else {
		argv = alloca(3 * sizeof(char *));
		argv[1] = job_get_string(j->ldj, LAUNCH_JOBKEY_PROGRAM);
		argv[2] = NULL;
	}

	if (job_get_string(j->ldj, LAUNCH_JOBKEY_PROGRAM))
		hasprog = true;

	if (inetcompat) {
		argv[0] = file2exec;
	} else {
		argv++;
		file2exec = job_get_file2exec(j->ldj);
	}

	if (hasprog) {
		r = execv(file2exec, (char *const*)argv);
	} else {
		r = execvp(file2exec, (char *const*)argv);
	}

	if (-1 == r) {
		write(execfd, &errno, sizeof(errno));
		job_log_error(j, LOG_ERR, "execv%s(\"%s\", ...)", hasprog ? "" : "p", file2exec);
	}
	exit(EXIT_FAILURE);
}

static void job_setup_attributes(struct jobcb *j)
{
	launch_data_t srl = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_SOFTRESOURCELIMITS);
	launch_data_t hrl = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_HARDRESOURCELIMITS);
	bool inetcompat = job_get_bool(j->ldj, LAUNCH_JOBKEY_INETDCOMPATIBILITY);
	launch_data_t tmp;
	size_t i;
	const char *tmpstr;
	struct group *gre = NULL;
	gid_t gre_g = 0;
	static const struct {
		const char *key;
		int val;
	} limits[] = {
		{ LAUNCH_JOBKEY_RESOURCELIMIT_CORE,    RLIMIT_CORE    },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_CPU,     RLIMIT_CPU     },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_DATA,    RLIMIT_DATA    },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_FSIZE,   RLIMIT_FSIZE   },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_MEMLOCK, RLIMIT_MEMLOCK },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_NOFILE,  RLIMIT_NOFILE  },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_NPROC,   RLIMIT_NPROC   },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_RSS,     RLIMIT_RSS     },
		{ LAUNCH_JOBKEY_RESOURCELIMIT_STACK,   RLIMIT_STACK   },
	};

	setpriority(PRIO_PROCESS, 0, job_get_integer(j->ldj, LAUNCH_JOBKEY_NICE));

	if (srl || hrl) {
		for (i = 0; i < (sizeof(limits) / sizeof(limits[0])); i++) {
			struct rlimit rl;

			if (getrlimit(limits[i].val, &rl) == -1) {
				job_log_error(j, LOG_WARNING, "getrlimit()");
				continue;
			}

			if (hrl)
				rl.rlim_max = job_get_integer(hrl, limits[i].key);
			if (srl)
				rl.rlim_cur = job_get_integer(srl, limits[i].key);

			if (setrlimit(limits[i].val, &rl) == -1)
				job_log_error(j, LOG_WARNING, "setrlimit()");
		}
	}

	if (!inetcompat && job_get_bool(j->ldj, LAUNCH_JOBKEY_SESSIONCREATE))
		launchd_SessionCreate(job_get_file2exec(j->ldj));

	if (job_get_bool(j->ldj, LAUNCH_JOBKEY_LOWPRIORITYIO)) {
		int lowprimib[] = { CTL_KERN, KERN_PROC_LOW_PRI_IO };
		int val = 1;

		if (sysctl(lowprimib, sizeof(lowprimib) / sizeof(lowprimib[0]), NULL, NULL,  &val, sizeof(val)) == -1)
			job_log_error(j, LOG_WARNING, "sysctl(\"%s\")", "kern.proc_low_pri_io");
	}
	if ((tmpstr = job_get_string(j->ldj, LAUNCH_JOBKEY_ROOTDIRECTORY))) {
		chroot(tmpstr);
		chdir(".");
	}
	if ((tmpstr = job_get_string(j->ldj, LAUNCH_JOBKEY_GROUPNAME))) {
		gre = getgrnam(tmpstr);
		if (gre) {
			gre_g = gre->gr_gid;
			if (-1 == setgid(gre_g)) {
				job_log_error(j, LOG_ERR, "setgid(%d)", gre_g);
				exit(EXIT_FAILURE);
			}
		} else {
			job_log(j, LOG_ERR, "getgrnam(\"%s\") failed", tmpstr);
			exit(EXIT_FAILURE);
		}
	}
	if ((tmpstr = job_get_string(j->ldj, LAUNCH_JOBKEY_USERNAME))) {
		struct passwd *pwe = getpwnam(tmpstr);
		if (pwe) {
			uid_t pwe_u = pwe->pw_uid;
			uid_t pwe_g = pwe->pw_gid;

			if (pwe->pw_expire && time(NULL) >= pwe->pw_expire) {
				job_log(j, LOG_ERR, "expired account: %s", tmpstr);
				exit(EXIT_FAILURE);
			}
			if (job_get_bool(j->ldj, LAUNCH_JOBKEY_INITGROUPS)) {
				if (-1 == initgroups(tmpstr, gre ? gre_g : pwe_g)) {
					job_log_error(j, LOG_ERR, "initgroups()");
					exit(EXIT_FAILURE);
				}
			}
			if (!gre) {
				if (-1 == setgid(pwe_g)) {
					job_log_error(j, LOG_ERR, "setgid(%d)", pwe_g);
					exit(EXIT_FAILURE);
				}
			}
			if (-1 == setuid(pwe_u)) {
				job_log_error(j, LOG_ERR, "setuid(%d)", pwe_u);
				exit(EXIT_FAILURE);
			}
		} else {
			job_log(j, LOG_WARNING, "getpwnam(\"%s\") failed", tmpstr);
			exit(EXIT_FAILURE);
		}
	}
	if ((tmpstr = job_get_string(j->ldj, LAUNCH_JOBKEY_WORKINGDIRECTORY)))
		chdir(tmpstr);
	if (launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_UMASK))
		umask(job_get_integer(j->ldj, LAUNCH_JOBKEY_UMASK));
	if ((tmpstr = job_get_string(j->ldj, LAUNCH_JOBKEY_STANDARDOUTPATH))) {
		int sofd = open(tmpstr, O_WRONLY|O_APPEND|O_CREAT, DEFFILEMODE);
		if (sofd == -1) {
			job_log_error(j, LOG_WARNING, "open(\"%s\", ...)", tmpstr);
		} else {
			dup2(sofd, STDOUT_FILENO);
			close(sofd);
		}
	}
	if ((tmpstr = job_get_string(j->ldj, LAUNCH_JOBKEY_STANDARDERRORPATH))) {
		int sefd = open(tmpstr, O_WRONLY|O_APPEND|O_CREAT, DEFFILEMODE);
		if (sefd == -1) {
			job_log_error(j, LOG_WARNING, "open(\"%s\", ...)", tmpstr);
		} else {
			dup2(sefd, STDERR_FILENO);
			close(sefd);
		}
	}
	if ((tmp = launch_data_dict_lookup(j->ldj, LAUNCH_JOBKEY_ENVIRONMENTVARIABLES)))
		launch_data_dict_iterate(tmp, setup_job_env, NULL);

	setsid();
}

#ifdef PID1_REAP_ADOPTED_CHILDREN
__private_extern__ int pid1_child_exit_status = 0;
static void pid1waitpid(void)
{
	pid_t p;

	while ((p = waitpid(-1, &pid1_child_exit_status, WNOHANG)) > 0) {
		if (!launchd_check_pid(p))
			init_check_pid(p);
	}
}
#endif

static void do_shutdown(void)
{
	struct jobcb *j;

	shutdown_in_progress = true;

	kevent_mod(asynckq, EVFILT_READ, EV_DISABLE, 0, 0, &kqasync_callback);

	TAILQ_FOREACH(j, &jobs, tqe)
		job_stop(j);

	if (getpid() == 1) {
		catatonia();
		mach_start_shutdown(SIGTERM);
	}
}

static void signal_callback(void *obj __attribute__((unused)), struct kevent *kev)
{
	switch (kev->ident) {
	case SIGHUP:
		update_ttys();
		reload_launchd_config();
		break;
	case SIGTERM:
		do_shutdown();
		break;
#ifdef PID1_REAP_ADOPTED_CHILDREN
	case SIGCHLD:
		/* <rdar://problem/3632556> Please automatically reap processes reparented to PID 1 */
		if (getpid() == 1) 
			pid1waitpid();
		break;
#endif
	default:
		break;
	} 
}

static void fs_callback(void)
{
	static bool mounted_volfs = false;

	if (1 != getpid())
		mounted_volfs = true;

	if (pending_stdout) {
		int fd = open(pending_stdout, O_CREAT|O_APPEND|O_WRONLY, DEFFILEMODE);
		if (fd != -1) {
			dup2(fd, STDOUT_FILENO);
			close(fd);
			free(pending_stdout);
			pending_stdout = NULL;
		}
	}
	if (pending_stderr) {
		int fd = open(pending_stderr, O_CREAT|O_APPEND|O_WRONLY, DEFFILEMODE);
		if (fd != -1) {
			dup2(fd, STDERR_FILENO);
			close(fd);
			free(pending_stderr);
			pending_stderr = NULL;
		}
	}

	if (!mounted_volfs) {
		int r = mount("volfs", VOLFSDIR, MNT_RDONLY, NULL);

		if (-1 == r && errno == ENOENT) {
			mkdir(VOLFSDIR, ACCESSPERMS & ~(S_IWUSR|S_IWGRP|S_IWOTH));
			r = mount("volfs", VOLFSDIR, MNT_RDONLY, NULL);
		}

		if (-1 == r) {
			syslog(LOG_WARNING, "mount(\"%s\", \"%s\", ...): %m", "volfs", VOLFSDIR);
		} else {
			mounted_volfs = true;
		}
	}

	if (!launchd_inited)
		launchd_server_init(false);
}

static void readcfg_callback(void *obj __attribute__((unused)), struct kevent *kev __attribute__((unused)))
{
	int status;

#ifdef PID1_REAP_ADOPTED_CHILDREN
	if (getpid() == 1)
		status = pid1_child_exit_status;
	else
#endif
	if (-1 == waitpid(readcfg_pid, &status, 0)) {
		syslog(LOG_WARNING, "waitpid(readcfg_pid, ...): %m");
		return;
	}

	readcfg_pid = 0;

	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status))
			syslog(LOG_WARNING, "Unable to read launchd.conf: launchctl exited with status: %d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		syslog(LOG_WARNING, "Unable to read launchd.conf: launchctl exited abnormally: %s", strsignal(WTERMSIG(status)));
	} else {
		syslog(LOG_WARNING, "Unable to read launchd.conf: launchctl exited abnormally");
	}
}

#ifdef EVFILT_MACH_IMPLEMENTED
static void *mach_demand_loop(void *arg __attribute__((unused)))
{
	mach_msg_empty_rcv_t dummy;
	kern_return_t kr;
	mach_port_name_array_t members;
	mach_msg_type_number_t membersCnt;
	mach_port_status_t status;
	mach_msg_type_number_t statusCnt;
	unsigned int i;

	for (;;) {

		/*
		 * Receive indication of message on demand service
		 * ports without actually receiving the message (we'll
		 * let the actual server do that.
		 */
		kr = mach_msg(&dummy.header, MACH_RCV_MSG|MACH_RCV_LARGE,
				0, 0, mach_demand_port_set, 0, MACH_PORT_NULL);
		if (kr != MACH_RCV_TOO_LARGE) {
			syslog(LOG_WARNING, "%s(): mach_msg(): %s", __func__, mach_error_string(kr));
			continue;
		}

		/*
		 * Some port(s) now have messages on them, find out
		 * which ones (there is no indication of which port
		 * triggered in the MACH_RCV_TOO_LARGE indication).
		 */
		kr = mach_port_get_set_status(mach_task_self(),
				mach_demand_port_set, &members, &membersCnt);
		if (kr != KERN_SUCCESS) {
			syslog(LOG_WARNING, "%s(): mach_port_get_set_status(): %s", __func__, mach_error_string(kr));
			continue;
		}

		for (i = 0; i < membersCnt; i++) {
			statusCnt = MACH_PORT_RECEIVE_STATUS_COUNT;
			kr = mach_port_get_attributes(mach_task_self(), members[i],
					MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&status, &statusCnt);
			if (kr != KERN_SUCCESS) {
				syslog(LOG_WARNING, "%s(): mach_port_get_attributes(): %s", __func__, mach_error_string(kr));
				continue;
			}

			/*
			 * For each port with messages, take it out of the
			 * demand service portset, and inform the main thread
			 * that it might have to start the server responsible
			 * for it.
			 */
			if (status.mps_msgcount) {
				kr = mach_port_move_member(mach_task_self(), members[i], MACH_PORT_NULL);
				if (kr != KERN_SUCCESS) {
					syslog(LOG_WARNING, "%s(): mach_port_move_member(): %s", __func__, mach_error_string(kr));
					continue;
				}
				write(machcbwritefd, &(members[i]), sizeof(members[i]));
			}
		}

		kr = vm_deallocate(mach_task_self(), (vm_address_t) members,
				(vm_size_t) membersCnt * sizeof(mach_port_name_t));
		if (kr != KERN_SUCCESS) {
			syslog(LOG_WARNING, "%s(): vm_deallocate(): %s", __func__, mach_error_string(kr));
			continue;
		}
	}

	return NULL;
}
#endif

static void reload_launchd_config(void)
{
	struct stat sb;
	static char *ldconf = PID1LAUNCHD_CONF;
	const char *h = getenv("HOME");

	if (h && ldconf == PID1LAUNCHD_CONF)
		asprintf(&ldconf, "%s/%s", h, LAUNCHD_CONF);

	if (!ldconf)
		return;

	if (lstat(ldconf, &sb) == 0) {
		int spair[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, spair);
		readcfg_pid = fork_with_bootstrap_port(launchd_bootstrap_port);
		if (readcfg_pid == 0) {
			char nbuf[100];
			close(spair[0]);
			sprintf(nbuf, "%d", spair[1]);
			setenv(LAUNCHD_TRUSTED_FD_ENV, nbuf, 1);
			int fd = open(ldconf, O_RDONLY);
			if (fd == -1) {
				syslog(LOG_ERR, "open(\"%s\"): %m", ldconf);
				exit(EXIT_FAILURE);
			}
			dup2(fd, STDIN_FILENO);
			close(fd);
			execl(LAUNCHCTL_PATH, LAUNCHCTL_PATH, NULL);
			syslog(LOG_ERR, "execl(\"%s\", ...): %m", LAUNCHCTL_PATH);
			exit(EXIT_FAILURE);
		} else if (readcfg_pid == -1) {
			close(spair[0]);
			close(spair[1]);
			syslog(LOG_ERR, "fork(): %m");
			readcfg_pid = 0;
		} else {
			close(spair[1]);
			ipc_open(_fd(spair[0]), NULL);
			if (kevent_mod(readcfg_pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &kqreadcfg_callback) == -1)
				syslog(LOG_ERR, "kevent_mod(EVFILT_PROC, &kqreadcfg_callback): %m");
		}
	}
}

static void conceive_firstborn(char *argv[])
{
	launch_data_t r, d = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_t args = launch_data_alloc(LAUNCH_DATA_ARRAY);
	launch_data_t l = launch_data_new_string("com.apple.launchd.firstborn");
	size_t i;

	for (i = 0; *argv; argv++, i++)
		launch_data_array_set_index(args, launch_data_new_string(*argv), i);

	launch_data_dict_insert(d, args, LAUNCH_JOBKEY_PROGRAMARGUMENTS);
	launch_data_dict_insert(d, l, LAUNCH_JOBKEY_LABEL);

	r = load_job(d);

	launch_data_free(r);
	launch_data_free(d);

	TAILQ_FIRST(&jobs)->firstborn = true;
}

static void loopback_setup(void)
{
	struct ifaliasreq ifra;
	struct in6_aliasreq ifra6;
	struct ifreq ifr;
	int s, s6;

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if (-1 == (s = socket(AF_INET, SOCK_DGRAM, 0)))
		syslog(LOG_ERR, "%s: socket(%s, ...): %m", __PRETTY_FUNCTION__, "AF_INET");
	if (-1 == (s6 = socket(AF_INET6, SOCK_DGRAM, 0)))
		syslog(LOG_ERR, "%s: socket(%s, ...): %m", __PRETTY_FUNCTION__, "AF_INET6");

	if (ioctl(s, SIOCGIFFLAGS, &ifr) == -1) {
		syslog(LOG_ERR, "ioctl(SIOCGIFFLAGS): %m");
	} else {
		ifr.ifr_flags |= IFF_UP;

		if (ioctl(s, SIOCSIFFLAGS, &ifr) == -1)
			syslog(LOG_ERR, "ioctl(SIOCSIFFLAGS): %m");
	}

	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, "lo0");

	if (ioctl(s6, SIOCGIFFLAGS, &ifr) == -1) {
		syslog(LOG_ERR, "ioctl(SIOCGIFFLAGS): %m");
	} else {
		ifr.ifr_flags |= IFF_UP;

		if (ioctl(s6, SIOCSIFFLAGS, &ifr) == -1)
			syslog(LOG_ERR, "ioctl(SIOCSIFFLAGS): %m");
	}

	memset(&ifra, 0, sizeof(ifra));
	strcpy(ifra.ifra_name, "lo0");

	((struct sockaddr_in *)&ifra.ifra_addr)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	((struct sockaddr_in *)&ifra.ifra_addr)->sin_len = sizeof(struct sockaddr_in);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_family = AF_INET;
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_addr.s_addr = htonl(IN_CLASSA_NET);
	((struct sockaddr_in *)&ifra.ifra_mask)->sin_len = sizeof(struct sockaddr_in);

	if (ioctl(s, SIOCAIFADDR, &ifra) == -1)
		syslog(LOG_ERR, "ioctl(SIOCAIFADDR ipv4): %m");

	memset(&ifra6, 0, sizeof(ifra6));
	strcpy(ifra6.ifra_name, "lo0");

	ifra6.ifra_addr.sin6_family = AF_INET6;
	ifra6.ifra_addr.sin6_addr = in6addr_loopback;
	ifra6.ifra_addr.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_prefixmask.sin6_family = AF_INET6;
	memset(&ifra6.ifra_prefixmask.sin6_addr, 0xff, sizeof(struct in6_addr));
	ifra6.ifra_prefixmask.sin6_len = sizeof(struct sockaddr_in6);
	ifra6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;
	ifra6.ifra_lifetime.ia6t_pltime = ND6_INFINITE_LIFETIME;

	if (ioctl(s6, SIOCAIFADDR_IN6, &ifra6) == -1)
		syslog(LOG_ERR, "ioctl(SIOCAIFADDR ipv6): %m");
 
	close(s);
	close(s6);
}

static void workaround3048875(int argc, char *argv[])
{
	int i;
	char **ap, *newargv[100], *p = argv[1];

	if (argc == 1 || argc > 2)
		return;

	newargv[0] = argv[0];
	for (ap = newargv + 1, i = 1; ap < &newargv[100]; ap++, i++) {
		if ((*ap = strsep(&p, " \t")) == NULL)
			break;
		if (**ap == '\0') {
			*ap = NULL;
			break;
		}
	}

	if (argc == i)
		return;

	execv(newargv[0], newargv);
}

static launch_data_t adjust_rlimits(launch_data_t in)
{
	static struct rlimit *l = NULL;
	static size_t lsz = sizeof(struct rlimit) * RLIM_NLIMITS;
	struct rlimit *ltmp;
	size_t i,ltmpsz;

	if (l == NULL) {
		l = malloc(lsz);
		for (i = 0; i < RLIM_NLIMITS; i++) {
			if (getrlimit(i, l + i) == -1)
				syslog(LOG_WARNING, "getrlimit(): %m");
		}
	}

	if (in) {
		ltmp = launch_data_get_opaque(in);
		ltmpsz = launch_data_get_opaque_size(in);

		if (ltmpsz > lsz) {
			syslog(LOG_WARNING, "Too much rlimit data sent!");
			ltmpsz = lsz;
		}
		
		for (i = 0; i < (ltmpsz / sizeof(struct rlimit)); i++) {
			if (ltmp[i].rlim_cur == l[i].rlim_cur && ltmp[i].rlim_max == l[i].rlim_max)
				continue;

			if (readcfg_pid && getpid() == 1) {
				int gmib[] = { CTL_KERN, KERN_MAXPROC };
				int pmib[] = { CTL_KERN, KERN_MAXPROCPERUID };
				const char *gstr = "kern.maxproc";
				const char *pstr = "kern.maxprocperuid";
				int gval = ltmp[i].rlim_max;
				int pval = ltmp[i].rlim_cur;
				switch (i) {
				case RLIMIT_NOFILE:
					gmib[1] = KERN_MAXFILES;
					pmib[1] = KERN_MAXFILESPERPROC;
					gstr = "kern.maxfiles";
					pstr = "kern.maxfilesperproc";
					break;
				case RLIMIT_NPROC:
					/* kernel will not clamp to this value, we must */
					if (gval > (2048 + 20))
						gval = 2048 + 20;
					break;
				default:
					break;
				}
				if (sysctl(gmib, 2, NULL, NULL, &gval, sizeof(gval)) == -1)
					syslog(LOG_WARNING, "sysctl(\"%s\"): %m", gstr);
				if (sysctl(pmib, 2, NULL, NULL, &pval, sizeof(pval)) == -1)
					syslog(LOG_WARNING, "sysctl(\"%s\"): %m", pstr);
			}
			if (setrlimit(i, ltmp + i) == -1)
				syslog(LOG_WARNING, "setrlimit(): %m");
			/* the kernel may have clamped the values we gave it */
			if (getrlimit(i, l + i) == -1)
				syslog(LOG_WARNING, "getrlimit(): %m");
		}
	}

	return launch_data_new_opaque(l, sizeof(struct rlimit) * RLIM_NLIMITS);
}

__private_extern__ void launchd_SessionCreate(const char *who)
{
	void *seclib = dlopen(SECURITY_LIB, RTLD_LAZY);
	OSStatus (*sescr)(SessionCreationFlags flags, SessionAttributeBits attributes);

	if (seclib) {
		sescr = dlsym(seclib, "SessionCreate");
		
		if (sescr) {
			OSStatus scr = sescr(0, 0);
			if (scr != noErr)
				syslog(LOG_WARNING, "%s: SessionCreate() failed: %d", who, scr);
		} else {
			syslog(LOG_WARNING, "%s: couldn't find SessionCreate() in %s", who, SECURITY_LIB);
		}

		dlclose(seclib);
	} else {
		syslog(LOG_WARNING, "%s: dlopen(\"%s\",...): %s", who, SECURITY_LIB, dlerror());
	}
}

static int dir_has_files(const char *path)
{
	DIR *dd = opendir(path);
	struct dirent *de;
	bool r = 0;

	if (!dd)
		return -1;

	while ((de = readdir(dd))) {
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
			r = 1;
			break;
		}
	}

	closedir(dd);
	return r;
}

static void job_set_alarm(struct jobcb *j)
{
	struct tm otherlatertm, latertm, *nowtm;
	time_t later, otherlater = 0, now = time(NULL);

	nowtm = localtime(&now);

	latertm = *nowtm;

	latertm.tm_sec = 0;
	latertm.tm_isdst = -1;


	if (-1 != j->start_cal_interval->tm_min)
		latertm.tm_min = j->start_cal_interval->tm_min;
	if (-1 != j->start_cal_interval->tm_hour)
		latertm.tm_hour = j->start_cal_interval->tm_hour;

	otherlatertm = latertm;

	if (-1 != j->start_cal_interval->tm_mday)
		latertm.tm_mday = j->start_cal_interval->tm_mday;
	if (-1 != j->start_cal_interval->tm_mon)
		latertm.tm_mon = j->start_cal_interval->tm_mon;

	/* cron semantics are fun */
	if (-1 != j->start_cal_interval->tm_wday) {
		int delta, realwday = j->start_cal_interval->tm_wday;

		if (realwday == 7)
			realwday = 0;
		
		delta = realwday - nowtm->tm_wday;
		
		/* Now Later Delta Desired
		 *   0     6     6       6
		 *   6     0    -6  7 + -6
		 *   1     5     4       4
		 *   5     1    -4  7 + -4
		 */
		if (delta > 0)
			otherlatertm.tm_mday += delta;
		else if (delta < 0)
			otherlatertm.tm_mday += 7 + delta;
		else if (-1 != j->start_cal_interval->tm_hour && otherlatertm.tm_hour <= nowtm->tm_hour)
			otherlatertm.tm_mday += 7;
		else if (-1 != j->start_cal_interval->tm_min && otherlatertm.tm_min <= nowtm->tm_min)
			otherlatertm.tm_hour++;
		else
			otherlatertm.tm_min++;

		otherlater = mktime(&otherlatertm);
	}

	if (-1 != j->start_cal_interval->tm_mon && latertm.tm_mon <= nowtm->tm_mon) {
		latertm.tm_year++;
	} else if (-1 != j->start_cal_interval->tm_mday && latertm.tm_mday <= nowtm->tm_mday) {
		latertm.tm_mon++;
	} else if (-1 != j->start_cal_interval->tm_hour && latertm.tm_hour <= nowtm->tm_hour) {
		latertm.tm_mday++;
	} else if (-1 != j->start_cal_interval->tm_min && latertm.tm_min <= nowtm->tm_min) {
		latertm.tm_hour++;
	} else {
		latertm.tm_min++;
	}

	later = mktime(&latertm);

	if (otherlater) {
		if (-1 != j->start_cal_interval->tm_mday)
			later = later < otherlater ? later : otherlater;
		else
			later = otherlater;
	}

	if (-1 == kevent_mod((uintptr_t)j->start_cal_interval, EVFILT_TIMER, EV_ADD, NOTE_ABSOLUTE|NOTE_SECONDS, later, &j->kqjob_callback))
		job_log_error(j, LOG_ERR, "adding kevent alarm");
}

static void job_log_error(struct jobcb *j, int pri, const char *msg, ...)
{
	size_t newmsg_sz = strlen(msg) + strlen(j->label) + 200;
	char *newmsg = alloca(newmsg_sz);
	va_list ap;

	sprintf(newmsg, "%s: %s: %s", j->label, msg, strerror(errno));

	va_start(ap, msg);

	vsyslog(pri, newmsg, ap);

	va_end(ap);
}

static void job_log(struct jobcb *j, int pri, const char *msg, ...)
{
	size_t newmsg_sz = strlen(msg) + sizeof(": ") + strlen(j->label);
	char *newmsg = alloca(newmsg_sz);
	va_list ap;

	sprintf(newmsg, "%s: %s", j->label, msg);

	va_start(ap, msg);

	vsyslog(pri, newmsg, ap);

	va_end(ap);
}

static void async_callback(void)
{
	struct timespec timeout = { 0, 0 };
	struct kevent kev;

	switch (kevent(asynckq, NULL, 0, &kev, 1, &timeout)) {
	case -1:
		syslog(LOG_DEBUG, "kevent(): %m");
		break;
	case 1:
		(*((kq_callback *)kev.udata))(kev.udata, &kev);
	case 0:
		break;
	default:
		syslog(LOG_DEBUG, "unexpected: kevent() returned something != 0, -1 or 1");
	}
}

static void testfd_or_openfd(int fd, const char *path, int flags)
{
	int tmpfd;

	if (-1 != (tmpfd = dup(fd))) {
		close(tmpfd);
	} else {
		if (-1 == (tmpfd = open(path, flags))) {
			syslog(LOG_ERR, "open(\"%s\", ...): %m", path);
		} else if (tmpfd != fd) {
			dup2(tmpfd, fd);
			close(tmpfd);
		}
	}
}
