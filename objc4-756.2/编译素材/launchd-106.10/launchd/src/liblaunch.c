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
#include <libkern/OSByteOrder.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include "launch.h"
#include "launch_priv.h"

/* __OSBogusByteSwap__() must not really exist in the symbol namespace
 * in order for the following to generate an error at build time.
 */
extern void __OSBogusByteSwap__(void);

#define host2big(x)				\
	({ typeof (x) _X, _x = (x);		\
	 switch (sizeof(_x)) {			\
	 case 8:				\
	 	_X = OSSwapHostToBigInt64(_x);	\
	 	break;				\
	 case 4:				\
	 	_X = OSSwapHostToBigInt32(_x);	\
	 	break;				\
	 case 2:				\
	 	_X = OSSwapHostToBigInt16(_x);	\
	 	break;				\
	 case 1:				\
	 	_X = _x;			\
		break;				\
	 default:				\
	 	__OSBogusByteSwap__();		\
		break;				\
	 }					\
	 _X;					\
	 })


#define big2host(x)				\
	({ typeof (x) _X, _x = (x);		\
	 switch (sizeof(_x)) {			\
	 case 8:				\
	 	_X = OSSwapBigToHostInt64(_x);	\
	 	break;				\
	 case 4:				\
	 	_X = OSSwapBigToHostInt32(_x);	\
	 	break;				\
	 case 2:				\
	 	_X = OSSwapBigToHostInt16(_x);	\
	 	break;				\
	 case 1:				\
	 	_X = _x;			\
		break;				\
	 default:				\
	 	__OSBogusByteSwap__();		\
		break;				\
	 }					\
	 _X;					\
	 })


struct launch_msg_header {
	uint64_t magic;
	uint64_t len;
};

#define LAUNCH_MSG_HEADER_MAGIC 0xD2FEA02366B39A41ull

struct _launch_data {
	int type;
	union {
		struct {
			launch_data_t *_array;
			size_t _array_cnt;
		};
		struct {
			char *string;
			size_t string_len;
		};
		struct {
			void *opaque;
			size_t opaque_size;
		};
		int fd;
		int err;
		long long number;
		bool boolean;
		double float_num;
	};
};

struct _launch {
	void	*sendbuf;
	int	*sendfds;   
	void	*recvbuf;
	int	*recvfds;   
	size_t	sendlen;                
	size_t	sendfdcnt;                    
	size_t	recvlen;                                
	size_t	recvfdcnt;                                    
	int	fd;                                                     
};

static void make_msg_and_cmsg(launch_data_t, void **, size_t *, int **, size_t *);
static launch_data_t make_data(launch_t, size_t *, size_t *);
static int _fd(int fd);

static pthread_once_t _lc_once = PTHREAD_ONCE_INIT;

static struct _launch_client {
	pthread_mutex_t mtx;
	launch_t	l;
	launch_data_t	async_resp;
} *_lc = NULL;

static void launch_client_init(void)
{
	struct sockaddr_un sun;
	char *where = getenv(LAUNCHD_SOCKET_ENV);
	char *_launchd_fd = getenv(LAUNCHD_TRUSTED_FD_ENV);
	int r, dfd, lfd = -1, tries;
	
	_lc = calloc(1, sizeof(struct _launch_client));

	if (!_lc)
		return;

	pthread_mutex_init(&_lc->mtx, NULL);

	if (_launchd_fd) {
		lfd = strtol(_launchd_fd, NULL, 10);
		if ((dfd = dup(lfd)) >= 0) {
			close(dfd);
			_fd(lfd);
		} else {
			lfd = -1;
		}
		unsetenv(LAUNCHD_TRUSTED_FD_ENV);
	}
	if (lfd == -1) {
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		
		if (where)
			strncpy(sun.sun_path, where, sizeof(sun.sun_path));
		else
			snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/%u/sock", LAUNCHD_SOCK_PREFIX, getuid());

		if ((lfd = _fd(socket(AF_UNIX, SOCK_STREAM, 0))) == -1)
			goto out_bad;

		for (tries = 0; tries < 10; tries++) {
			r = connect(lfd, (struct sockaddr *)&sun, sizeof(sun));
			if (r == -1) {
				if (getuid() != 0 && fork() == 0)
					execl("/sbin/launchd", "/sbin/launchd", NULL);
				sleep(1);
			} else {
				break;
			}
		}
		if (r == -1) {
			close(lfd);
			goto out_bad;
		}
	}
	if (!(_lc->l = launchd_fdopen(lfd))) {
		close(lfd);
		goto out_bad;
	}
	if (!(_lc->async_resp = launch_data_alloc(LAUNCH_DATA_ARRAY)))
		goto out_bad;

	return;
out_bad:
	if (_lc->l)
		launchd_close(_lc->l);
	if (_lc)
		free(_lc);
	_lc = NULL;
}

launch_data_t launch_data_alloc(launch_data_type_t t)
{
	launch_data_t d = calloc(1, sizeof(struct _launch));

	if (d) {
		d->type = t;
		switch (t) {
		case LAUNCH_DATA_DICTIONARY:
		case LAUNCH_DATA_ARRAY:
			d->_array = malloc(0);
			break;
		default:
			break;
		}
	}

	return d;
}

launch_data_type_t launch_data_get_type(launch_data_t d)
{
	return d->type;
}

void launch_data_free(launch_data_t d)
{
	size_t i;

	switch (d->type) {
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		for (i = 0; i < d->_array_cnt; i++)
			launch_data_free(d->_array[i]);
		free(d->_array);
		break;
	case LAUNCH_DATA_STRING:
		if (d->string)
			free(d->string);
		break;
	case LAUNCH_DATA_OPAQUE:
		if (d->opaque)
			free(d->opaque);
		break;
	default:
		break;
	}
	free(d);
}

size_t launch_data_dict_get_count(launch_data_t dict)
{
	return dict->_array_cnt / 2;
}


bool launch_data_dict_insert(launch_data_t dict, launch_data_t what, const char *key)
{
	size_t i;
	launch_data_t thekey = launch_data_alloc(LAUNCH_DATA_STRING);

	launch_data_set_string(thekey, key);

	for (i = 0; i < dict->_array_cnt; i += 2) {
		if (!strcasecmp(key, dict->_array[i]->string)) {
			launch_data_array_set_index(dict, thekey, i);
			launch_data_array_set_index(dict, what, i + 1);
			return true;
		}
	}
	launch_data_array_set_index(dict, thekey, i);
	launch_data_array_set_index(dict, what, i + 1);
	return true;
}

launch_data_t launch_data_dict_lookup(launch_data_t dict, const char *key)
{
	size_t i;

	if (LAUNCH_DATA_DICTIONARY != dict->type)
		return NULL;

	for (i = 0; i < dict->_array_cnt; i += 2) {
		if (!strcasecmp(key, dict->_array[i]->string))
			return dict->_array[i + 1];
	}

	return NULL;
}

bool launch_data_dict_remove(launch_data_t dict, const char *key)
{
	size_t i;

	for (i = 0; i < dict->_array_cnt; i += 2) {
		if (!strcasecmp(key, dict->_array[i]->string))
			break;
	}
	if (i == dict->_array_cnt)
		return false;
	launch_data_free(dict->_array[i]);
	launch_data_free(dict->_array[i + 1]);
	memmove(dict->_array + i, dict->_array + i + 2, (dict->_array_cnt - (i + 2)) * sizeof(launch_data_t));
	dict->_array_cnt -= 2;
	return true;
}

void launch_data_dict_iterate(launch_data_t dict, void (*cb)(launch_data_t, const char *, void *), void *context)
{
	size_t i;

	if (LAUNCH_DATA_DICTIONARY != dict->type)
		return;

	for (i = 0; i < dict->_array_cnt; i += 2)
		cb(dict->_array[i + 1], dict->_array[i]->string, context);
}

bool launch_data_array_set_index(launch_data_t where, launch_data_t what, size_t ind)
{
	if ((ind + 1) >= where->_array_cnt) {
		where->_array = realloc(where->_array, (ind + 1) * sizeof(launch_data_t));
		memset(where->_array + where->_array_cnt, 0, (ind + 1 - where->_array_cnt) * sizeof(launch_data_t));
		where->_array_cnt = ind + 1;
	}

	if (where->_array[ind])
		launch_data_free(where->_array[ind]);
	where->_array[ind] = what;
	return true;
}

launch_data_t launch_data_array_get_index(launch_data_t where, size_t ind)
{
	if (LAUNCH_DATA_ARRAY != where->type)
		return NULL;
	if (ind < where->_array_cnt)
		return where->_array[ind];
	return NULL;
}

launch_data_t launch_data_array_pop_first(launch_data_t where)
{
	launch_data_t r = NULL;
       
	if (where->_array_cnt > 0) {
		r = where->_array[0];
		memmove(where->_array, where->_array + 1, (where->_array_cnt - 1) * sizeof(launch_data_t));
		where->_array_cnt--;
	}
	return r;
}

size_t launch_data_array_get_count(launch_data_t where)
{
	if (LAUNCH_DATA_ARRAY != where->type)
		return 0;
	return where->_array_cnt;
}

bool launch_data_set_errno(launch_data_t d, int e)
{
	d->err = e;
	return true;
}

bool launch_data_set_fd(launch_data_t d, int fd)
{
	d->fd = fd;
	return true;
}

bool launch_data_set_integer(launch_data_t d, long long n)
{
	d->number = n;
	return true;
}

bool launch_data_set_bool(launch_data_t d, bool b)
{
	d->boolean = b;
	return true;
}

bool launch_data_set_real(launch_data_t d, double n)
{
	d->float_num = n;
	return true;
}

bool launch_data_set_string(launch_data_t d, const char *s)
{
	if (d->string)
		free(d->string);
	d->string = strdup(s);
	if (d->string) {
		d->string_len = strlen(d->string);
		return true;
	}
	return false;
}

bool launch_data_set_opaque(launch_data_t d, const void *o, size_t os)
{
	d->opaque_size = os;
	if (d->opaque)
		free(d->opaque);
	d->opaque = malloc(os);
	if (d->opaque) {
		memcpy(d->opaque, o, os);
		return true;
	}
	return false;
}

int launch_data_get_errno(launch_data_t d)
{
	return d->err;
}

int launch_data_get_fd(launch_data_t d)
{
	return d->fd;
}

long long launch_data_get_integer(launch_data_t d)
{
	return d->number;
}

bool launch_data_get_bool(launch_data_t d)
{
	return d->boolean;
}

double launch_data_get_real(launch_data_t d)
{
	return d->float_num;
}

const char *launch_data_get_string(launch_data_t d)
{
	if (LAUNCH_DATA_STRING != d->type)
		return NULL;
	return d->string;
}

void *launch_data_get_opaque(launch_data_t d)
{
	if (LAUNCH_DATA_OPAQUE != d->type)
		return NULL;
	return d->opaque;
}

size_t launch_data_get_opaque_size(launch_data_t d)
{
	return d->opaque_size;
}

int launchd_getfd(launch_t l)
{
	return l->fd;
}

launch_t launchd_fdopen(int fd)
{
        launch_t c;

        c = calloc(1, sizeof(struct _launch));
	if (!c)
		return NULL;

        c->fd = fd;

	fcntl(fd, F_SETFL, O_NONBLOCK);

        if ((c->sendbuf = malloc(0)) == NULL)
		goto out_bad;
        if ((c->sendfds = malloc(0)) == NULL)
		goto out_bad;
        if ((c->recvbuf = malloc(0)) == NULL)
		goto out_bad;
        if ((c->recvfds = malloc(0)) == NULL)
		goto out_bad;

	return c;

out_bad:
	if (c->sendbuf)
		free(c->sendbuf);
	if (c->sendfds)
		free(c->sendfds);
	if (c->recvbuf)
		free(c->recvbuf);
	if (c->recvfds)
		free(c->recvfds);
	free(c);
	return NULL;
}

void launchd_close(launch_t lh)
{
	if (lh->sendbuf)
		free(lh->sendbuf);
	if (lh->sendfds)
		free(lh->sendfds);
	if (lh->recvbuf)
		free(lh->recvbuf);
	if (lh->recvfds)
		free(lh->recvfds);
	close(lh->fd);
	free(lh);
}

static void make_msg_and_cmsg(launch_data_t d, void **where, size_t *len, int **fd_where, size_t *fdcnt)
{
	launch_data_t o_in_w;
	size_t i;

	*where = realloc(*where, *len + sizeof(struct _launch_data));

	o_in_w = *where + *len;
	memset(o_in_w, 0, sizeof(struct _launch_data));
	*len += sizeof(struct _launch_data);

	o_in_w->type = host2big(d->type);

	switch (d->type) {
	case LAUNCH_DATA_INTEGER:
		o_in_w->number = host2big(d->number);
		break;
	case LAUNCH_DATA_REAL:
		o_in_w->float_num = host2big(d->float_num);
		break;
	case LAUNCH_DATA_BOOL:
		o_in_w->boolean = host2big(d->boolean);
		break;
	case LAUNCH_DATA_ERRNO:
		o_in_w->err = host2big(d->err);
		break;
	case LAUNCH_DATA_FD:
		o_in_w->fd = host2big(d->fd);
		if (d->fd != -1) {
			*fd_where = realloc(*fd_where, (*fdcnt + 1) * sizeof(int));
			(*fd_where)[*fdcnt] = d->fd;
			(*fdcnt)++;
		}
		break;
	case LAUNCH_DATA_STRING:
		o_in_w->string_len = host2big(d->string_len);
		*where = realloc(*where, *len + strlen(d->string) + 1);
		memcpy(*where + *len, d->string, strlen(d->string) + 1);
		*len += strlen(d->string) + 1;
		break;
	case LAUNCH_DATA_OPAQUE:
		o_in_w->opaque_size = host2big(d->opaque_size);
		*where = realloc(*where, *len + d->opaque_size);
		memcpy(*where + *len, d->opaque, d->opaque_size);
		*len += d->opaque_size;
		break;
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		o_in_w->_array_cnt = host2big(d->_array_cnt);
		*where = realloc(*where, *len + (d->_array_cnt * sizeof(launch_data_t)));
		memcpy(*where + *len, d->_array, d->_array_cnt * sizeof(launch_data_t));
		*len += d->_array_cnt * sizeof(launch_data_t);

		for (i = 0; i < d->_array_cnt; i++)
			make_msg_and_cmsg(d->_array[i], where, len, fd_where, fdcnt);
		break;
	default:
		break;
	}
}

static launch_data_t make_data(launch_t conn, size_t *data_offset, size_t *fdoffset)
{
	launch_data_t r = conn->recvbuf + *data_offset;
	size_t i, tmpcnt;

	if ((conn->recvlen - *data_offset) < sizeof(struct _launch_data))
		return NULL;
	*data_offset += sizeof(struct _launch_data);

	switch (big2host(r->type)) {
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		tmpcnt = big2host(r->_array_cnt);
		if ((conn->recvlen - *data_offset) < (tmpcnt * sizeof(launch_data_t))) {
			errno = EAGAIN;
			return NULL;
		}
		r->_array = conn->recvbuf + *data_offset;
		*data_offset += tmpcnt * sizeof(launch_data_t);
		for (i = 0; i < tmpcnt; i++) {
			r->_array[i] = make_data(conn, data_offset, fdoffset);
			if (r->_array[i] == NULL)
				return NULL;
		}
		r->_array_cnt = tmpcnt;
		break;
	case LAUNCH_DATA_STRING:
		tmpcnt = big2host(r->string_len);
		if ((conn->recvlen - *data_offset) < (tmpcnt + 1)) {
			errno = EAGAIN;
			return NULL;
		}
		r->string = conn->recvbuf + *data_offset;
		r->string_len = tmpcnt;
		*data_offset += tmpcnt + 1;
		break;
	case LAUNCH_DATA_OPAQUE:
		tmpcnt = big2host(r->opaque_size);
		if ((conn->recvlen - *data_offset) < tmpcnt) {
			errno = EAGAIN;
			return NULL;
		}
		r->opaque = conn->recvbuf + *data_offset;
		r->opaque_size = tmpcnt;
		*data_offset += tmpcnt;
		break;
	case LAUNCH_DATA_FD:
		if (r->fd != -1) {
			r->fd = _fd(conn->recvfds[*fdoffset]);
			*fdoffset += 1;
		}
		break;
	case LAUNCH_DATA_INTEGER:
		r->number = big2host(r->number);
		break;
	case LAUNCH_DATA_REAL:
		r->float_num = big2host(r->float_num);
		break;
	case LAUNCH_DATA_BOOL:
		r->boolean = big2host(r->boolean);
		break;
	case LAUNCH_DATA_ERRNO:
		r->err = big2host(r->err);
		break;
	default:
		errno = EINVAL;
		return NULL;
		break;
	}

	r->type = big2host(r->type);

	return r;
}

int launchd_msg_send(launch_t lh, launch_data_t d)
{
	struct launch_msg_header lmh;
	struct cmsghdr *cm = NULL;
	struct msghdr mh;
	struct iovec iov[2];
	size_t sentctrllen = 0;
	int r;

	memset(&mh, 0, sizeof(mh));

	if (d) {
		uint64_t msglen = lh->sendlen;

		make_msg_and_cmsg(d, &lh->sendbuf, &lh->sendlen, &lh->sendfds, &lh->sendfdcnt);

		msglen = (lh->sendlen - msglen) + sizeof(struct launch_msg_header);
		lmh.len = host2big(msglen);
		lmh.magic = host2big(LAUNCH_MSG_HEADER_MAGIC);

		iov[0].iov_base = &lmh;
		iov[0].iov_len = sizeof(lmh);
		mh.msg_iov = iov;
        	mh.msg_iovlen = 2;
	} else {
		mh.msg_iov = iov + 1;
        	mh.msg_iovlen = 1;
	}

	iov[1].iov_base = lh->sendbuf;
	iov[1].iov_len = lh->sendlen;


	if (lh->sendfdcnt > 0) {
		sentctrllen = mh.msg_controllen = CMSG_SPACE(lh->sendfdcnt * sizeof(int));
		cm = alloca(mh.msg_controllen);
		mh.msg_control = cm;

		memset(cm, 0, mh.msg_controllen);

		cm->cmsg_len = CMSG_LEN(lh->sendfdcnt * sizeof(int));
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;

		memcpy(CMSG_DATA(cm), lh->sendfds, lh->sendfdcnt * sizeof(int));
	}

	if ((r = sendmsg(lh->fd, &mh, 0)) == -1) {
		return -1;
	} else if (r == 0) {
		errno = ECONNRESET;
		return -1;
	} else if (sentctrllen != mh.msg_controllen) {
		errno = ECONNRESET;
		return -1;
	}

	if (d) {
		r -= sizeof(struct launch_msg_header);
	}

	lh->sendlen -= r;
	if (lh->sendlen > 0) {
		memmove(lh->sendbuf, lh->sendbuf + r, lh->sendlen);
	} else {
		free(lh->sendbuf);
		lh->sendbuf = malloc(0);
	}

	lh->sendfdcnt = 0;
	free(lh->sendfds);
	lh->sendfds = malloc(0);

	if (lh->sendlen > 0) {
		errno = EAGAIN;
		return -1;
	}

	return 0;
}


int launch_get_fd(void)
{
	pthread_once(&_lc_once, launch_client_init);

	if (!_lc) {
		errno = ENOTCONN;
		return -1;
	}

	return _lc->l->fd;
}

static void launch_msg_getmsgs(launch_data_t m, void *context)
{
	launch_data_t async_resp, *sync_resp = context;
	
	if ((LAUNCH_DATA_DICTIONARY == launch_data_get_type(m)) && (async_resp = launch_data_dict_lookup(m, LAUNCHD_ASYNC_MSG_KEY))) {
		launch_data_array_set_index(_lc->async_resp, launch_data_copy(async_resp), launch_data_array_get_count(_lc->async_resp));
	} else {
		*sync_resp = launch_data_copy(m);
	}
}

launch_data_t launch_msg(launch_data_t d)
{
	launch_data_t resp = NULL;

	pthread_once(&_lc_once, launch_client_init);

	if (!_lc) {
		errno = ENOTCONN;
		return NULL;
	}

	pthread_mutex_lock(&_lc->mtx);

	if (d && launchd_msg_send(_lc->l, d) == -1) {
		do {
			if (errno != EAGAIN)
				goto out;
		} while (launchd_msg_send(_lc->l, NULL) == -1);
	}
       
	while (resp == NULL) {
		if (d == NULL && launch_data_array_get_count(_lc->async_resp) > 0) {
			resp = launch_data_array_pop_first(_lc->async_resp);
			goto out;
		}
		if (launchd_msg_recv(_lc->l, launch_msg_getmsgs, &resp) == -1) {
			if (errno != EAGAIN) {
				goto out;
			} else if (d == NULL) {
				errno = 0;
				goto out;
			} else {
				fd_set rfds;

				FD_ZERO(&rfds);
				FD_SET(_lc->l->fd, &rfds);
			
				select(_lc->l->fd + 1, &rfds, NULL, NULL, NULL);
			}
		}
	}

out:
	pthread_mutex_unlock(&_lc->mtx);

	return resp;
}

int launchd_msg_recv(launch_t lh, void (*cb)(launch_data_t, void *), void *context)
{
	struct cmsghdr *cm = alloca(4096); 
	launch_data_t rmsg = NULL;
	size_t data_offset, fd_offset;
        struct msghdr mh;
        struct iovec iov;
	int r;

        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;

	lh->recvbuf = realloc(lh->recvbuf, lh->recvlen + 8*1024);

	iov.iov_base = lh->recvbuf + lh->recvlen;
	iov.iov_len = 8*1024;
	mh.msg_control = cm;
	mh.msg_controllen = 4096;

	if ((r = recvmsg(lh->fd, &mh, 0)) == -1)
		return -1;
	if (r == 0) {
		errno = ECONNRESET;
		return -1;
	}
	if (mh.msg_flags & MSG_CTRUNC) {
		errno = ECONNABORTED;
		return -1;
	}
	lh->recvlen += r;
	if (mh.msg_controllen > 0) {
		lh->recvfds = realloc(lh->recvfds, lh->recvfdcnt * sizeof(int) + mh.msg_controllen - sizeof(struct cmsghdr));
		memcpy(lh->recvfds + lh->recvfdcnt, CMSG_DATA(cm), mh.msg_controllen - sizeof(struct cmsghdr));
		lh->recvfdcnt += (mh.msg_controllen - sizeof(struct cmsghdr)) / sizeof(int);
	}

	r = 0;

	while (lh->recvlen > 0) {
		struct launch_msg_header *lmhp = lh->recvbuf;
		uint64_t tmplen;
		data_offset = sizeof(struct launch_msg_header);
		fd_offset = 0;

		if (lh->recvlen < sizeof(struct launch_msg_header))
			goto need_more_data;

		tmplen = big2host(lmhp->len);

		if (big2host(lmhp->magic) != LAUNCH_MSG_HEADER_MAGIC || tmplen <= sizeof(struct launch_msg_header)) {
			errno = EBADRPC;
			goto out_bad;
		}

		if (lh->recvlen < tmplen) {
			goto need_more_data;
		}

		if ((rmsg = make_data(lh, &data_offset, &fd_offset)) == NULL) {
			errno = EBADRPC;
			goto out_bad;
		}

		cb(rmsg, context);

		lh->recvlen -= data_offset;
		if (lh->recvlen > 0) {
			memmove(lh->recvbuf, lh->recvbuf + data_offset, lh->recvlen);
		} else {
			free(lh->recvbuf);
			lh->recvbuf = malloc(0);
		}

		lh->recvfdcnt -= fd_offset;
		if (lh->recvfdcnt > 0) {
			memmove(lh->recvfds, lh->recvfds + fd_offset, lh->recvfdcnt * sizeof(int));
		} else {
			free(lh->recvfds);
			lh->recvfds = malloc(0);
		}
	}

	return r;

need_more_data:
	errno = EAGAIN;
out_bad:
	return -1;
}

launch_data_t launch_data_copy(launch_data_t o)
{
	launch_data_t r = launch_data_alloc(o->type);
	size_t i;

	free(r->_array);
	memcpy(r, o, sizeof(struct _launch_data));

	switch (o->type) {
	case LAUNCH_DATA_DICTIONARY:
	case LAUNCH_DATA_ARRAY:
		r->_array = calloc(1, o->_array_cnt * sizeof(launch_data_t));
		for (i = 0; i < o->_array_cnt; i++) {
			if (o->_array[i])
				r->_array[i] = launch_data_copy(o->_array[i]);
		}
		break;
	case LAUNCH_DATA_STRING:
		r->string = strdup(o->string);
		break;
	case LAUNCH_DATA_OPAQUE:
		r->opaque = malloc(o->opaque_size);
		memcpy(r->opaque, o->opaque, o->opaque_size);
		break;
	default:
		break;
	}

	return r;
}

void launchd_batch_enable(bool val)
{
	launch_data_t resp, tmp, msg;

	tmp = launch_data_alloc(LAUNCH_DATA_BOOL);
	launch_data_set_bool(tmp, val);

	msg = launch_data_alloc(LAUNCH_DATA_DICTIONARY);
	launch_data_dict_insert(msg, tmp, LAUNCH_KEY_BATCHCONTROL);

	resp = launch_msg(msg);

	launch_data_free(msg);

	if (resp)
		launch_data_free(resp);
}

bool launchd_batch_query(void)
{
	launch_data_t resp, msg = launch_data_alloc(LAUNCH_DATA_STRING);
	bool rval = true;

	launch_data_set_string(msg, LAUNCH_KEY_BATCHQUERY);

	resp = launch_msg(msg);

	launch_data_free(msg);

	if (resp) {
		if (launch_data_get_type(resp) == LAUNCH_DATA_BOOL)
			rval = launch_data_get_bool(resp);
		launch_data_free(resp);
	}
	return rval;
}

static int _fd(int fd)
{
	if (fd >= 0)
		fcntl(fd, F_SETFD, 1);
	return fd;
}

launch_data_t launch_data_new_errno(int e)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_ERRNO);

	if (r)
	       launch_data_set_errno(r, e);

	return r;
}

launch_data_t launch_data_new_fd(int fd)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_FD);

	if (r)
	       launch_data_set_fd(r, fd);

	return r;
}

launch_data_t launch_data_new_integer(long long n)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_INTEGER);

	if (r)
		launch_data_set_integer(r, n);

	return r;
}

launch_data_t launch_data_new_bool(bool b)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_BOOL);

	if (r)
		launch_data_set_bool(r, b);

	return r;
}

launch_data_t launch_data_new_real(double d)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_REAL);

	if (r)
		launch_data_set_real(r, d);

	return r;
}

launch_data_t launch_data_new_string(const char *s)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_STRING);

	if (r == NULL)
		return NULL;

	if (!launch_data_set_string(r, s)) {
		launch_data_free(r);
		return NULL;
	}

	return r;
}

launch_data_t launch_data_new_opaque(const void *o, size_t os)
{
	launch_data_t r = launch_data_alloc(LAUNCH_DATA_OPAQUE);

	if (r == NULL)
		return NULL;

	if (!launch_data_set_opaque(r, o, os)) {
		launch_data_free(r);
		return NULL;
	}

	return r;
}
