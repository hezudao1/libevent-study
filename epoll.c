/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
struct evepoll {
	struct event *evread;
	struct event *evwrite;
};

struct epollop {
	struct evepoll *fds;        //evepoll���͵�����,��ʼ��СΪ32.
	int nfds;                   //��ʾfds��������Ԫ�ظ���. ��ʼֵΪ32.
	struct epoll_event *events; //epoll_event���͵�����,�ṩepollʹ��. ��ʼ��СΪ32, ���ֵΪ4096.
	int nevents;                //��ʾevents����Ĵ�С, ��ʼֵΪ32.
	int epfd;                   //����epoll_create�������ɵ��ļ�������.
};

static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

#define INITIAL_NFILES 32
#define INITIAL_NEVENTS 32
#define MAX_NEVENTS 4096

static void *
epoll_init(struct event_base *base)
{
	int epfd;
	struct epollop *epollop;

	/* Disable epollueue when this environment variable is set */
	if (evutil_getenv("EVENT_NOEPOLL"))
		return (NULL);

	/* Initalize the kernel queue */
	if ((epfd = epoll_create(32000)) == -1) {   //����һ��epoll���ļ�������.
		if (errno != ENOSYS)
			event_warn("epoll_create");
		return (NULL);
	}

	FD_CLOSEONEXEC(epfd);                       //����epfd��closeOnExec����.

	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;

	/* Initalize fields */                      //����events������.ע��������epoll_event
	epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		free(epollop);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENTS;
    //����evepoll������.
	epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);
		free(epollop);
		return (NULL);
	}
	epollop->nfds = INITIAL_NFILES;
    //����Ҫע����. ��ʼ�����źŴ���Ķ���. ֻ������ط���ʼ�����źŴ���Ķ���.
	evsignal_init(base);

	return (epollop);
}
//����������Ǹ�����չepollop�е�fds����, Ҳ����evepoll���͵�����.ʵ�ʾ��Ƕ�Ӧ��libevent�Ĵ���.ÿ������һ��,���ֵ�ǲ���max������.
static int
epoll_recalc(struct event_base *base, void *arg, int max)
{
	struct epollop *epollop = arg;

	if (max >= epollop->nfds) {
		struct evepoll *fds;
		int nfds;

		nfds = epollop->nfds;
		while (nfds <= max)
			nfds <<= 1;

		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		epollop->fds = fds;
		memset(fds + epollop->nfds, 0,
		    (nfds - epollop->nfds) * sizeof(struct evepoll));
		epollop->nfds = nfds;
	}

	return (0);
}

static int
epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv)
{
	struct epollop *epollop = arg;
	struct epoll_event *events = epollop->events;
	struct evepoll *evep;
	int i, res, timeout = -1;

	if (tv != NULL)
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;

	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {//epollϵͳ���õĵȴ���ʱ�䲻��̫��.�35����
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}
    //����epollϵͳ���þͿ�ʼwait��. ����ֵ��-1������, 0��ʱ��, ���� �����ļ��������ĸ���. timeout�ĵ�λ�Ǻ���.
	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base); //ֻҪepoll_wait������������, �ʹ����ź�event���뵽active������
		return (0);
	} else if (base->sig.evsignal_caught) {
		evsignal_process(base); //ֻҪ�źŷ����˾ʹ����ź�.�ʹ����ź�event���뵽active������
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));

	for (i = 0; i < res; i++) { //��ʼ������.
		int what = events[i].events;    //epoll_event�ṹ���е�events�ֶ�,��ʾ�������¼�����.
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;

		if (fd < 0 || fd >= epollop->nfds)
			continue;
		evep = &epollop->fds[fd];   //��������Կ���evepoll���͵�����fds��epoll_event���͵�����events�Ķ�Ӧ��ϵ.���Կ�������������Ĵ�С��һ�������, ��Ϊevents�����������epoll_wait��þ������ļ���������. �ļ���������̫���ܻ�ͬһʱ��ȫ������.
		//����Ϳ��Կ���, fds��������������ļ�������. ͨ���ļ�������, �����������.
		if (what & (EPOLLHUP|EPOLLERR)) {   //��һ�������ļ�������, ��Ӧ���ļ����������Ҷ�,��Ӧ���ļ�������������.���ö�д�¼�.
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			if (what & EPOLLIN) {//�ļ��������Ƕ��¼�.
				evread = evep->evread;
			}

			if (what & EPOLLOUT) {//�ļ���������д�¼�.
				evwrite = evep->evwrite;
			}
		}

		if (!(evread||evwrite))
			continue;

		if (evread != NULL) //�Ѷ�Ӧ��read��event�ṹ��,���뵽libevent��active������.
			event_active(evread, EV_READ, 1);
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);
	}

	if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {    //��������epoll_event���͵�����event�ǲ��ǲ�������.�Ǿ���չ��.
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}


static int
epoll_add(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, op, events;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_add(ev));  //�����event���ź����͵��¼�,�ͻ�ֱ�Ӳ��뵽�źŶ�Ӧ����Ϣ������.

	fd = ev->ev_fd;
	if (fd >= epollop->nfds) {
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)   //������չevepoll��������fds�Ĵ�С.��Ϊ���ڳ��ֵ��ļ�������������fds�Ĵ�С.
			return (-1);
	}
	evep = &epollop->fds[fd];   //��ȡ�������еĶ�Ӧ��Ԫ��.
	op = EPOLL_CTL_ADD;         //Ĭ�ϵĲ�����EPOLL_CTL_ADD.
	events = 0;
	if (evep->evread != NULL) { //��evep�Ķ�����дevent��Ϊ��. ���Ҫ����ԭ���ļ����¼�, ҲҪ�����µ��¼�,���Բ���Ҫ�ĳ�MOD.
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	if (evep->evwrite != NULL) {    //����ԭ���ļ����¼�.
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}

	if (ev->ev_events & EV_READ)    //��ʼ�����µļ����¼�.
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)   //��ʼ�����µļ����¼�.
		events |= EPOLLOUT;

	epev.data.fd = fd;              //�����ļ�������
	epev.events = events;           //��������¼�.
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)   //��ʼ�޸�epoll�ļ����¼�.
			return (-1);

	/* Update events responsible */
	if (ev->ev_events & EV_READ)    //�⼸�����Ǹ���evepoll�����е�Ԫ�ص���Ϣ. ֻ��epoll_ctl�ɹ�֮��Ÿ��µ�.����.
		evep->evread = ev;
	if (ev->ev_events & EV_WRITE)
		evep->evwrite = ev;

	return (0);
}

static int
epoll_del(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, events, op;
	int needwritedelete = 1, needreaddelete = 1;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_del(ev));  //ɾ����Ӧ���źŵ���Ϣ.�Ӷ�Ӧ�źŵ��¼�������ɾ����.

	fd = ev->ev_fd;
	if (fd >= epollop->nfds)        //������event��Ӧ���ļ�������, �����˹�����evepoll������ķ�Χ.
		return (0);
	evep = &epollop->fds[fd];

	op = EPOLL_CTL_DEL;
	events = 0;

	if (ev->ev_events & EV_READ)    //��event�ж��¼�, ������events��־λ.
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)   //��event��д�¼�, ������events��־λ.
		events |= EPOLLOUT;

	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {  //��event�ȼ����˶��ּ�����д,�ǾͿ�����ȫɾ����.
		if ((events & EPOLLIN) && evep->evwrite != NULL) {      //���eventֻ�����˶��¼�, �������event��Ӧ��fd, fd������evepoll�ṹ�е�writeevent ��Ϊ��, ��ô�����������.
			needwritedelete = 0;    //��epoll����д�¼�,���������¼�. ��ɾ����Ӧ�ı�־λ.
			events = EPOLLOUT;      //����event�ı�ʶλ.
			op = EPOLL_CTL_MOD;     //��Ϊ֮ǰevent�Ѿ������õ�epoll�ļ���������.����, ��������EPOLL_CTL_ADD, ����EPOLL_CTL_MOD. �Ѷ�������Ϊд����. ��ʵ�����һ����Ե���.
		} else if ((events & EPOLLOUT) && evep->evread != NULL) {   //ͬ��������. Ҳ��һ�ֱ�Ե���.
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.fd = fd;
    //�����Ӧ���¶�Ӧ��evepoll�ṹ���е�����.
	if (needreaddelete)
		evep->evread = NULL;
	if (needwritedelete)
		evep->evwrite = NULL;
    //
	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)  //���õײ��API��.
		return (-1);

	return (0);
}

static void
epoll_dealloc(struct event_base *base, void *arg)
{
	struct epollop *epollop = arg;

	evsignal_dealloc(base);
	if (epollop->fds)
		free(epollop->fds);     //�ͷ�epollop�е�evepoll���͵Ľṹ������.
	if (epollop->events)
		free(epollop->events);  //�ͷ�epollop�е�epoll_event���͵Ľṹ������.
	if (epollop->epfd >= 0)
		close(epollop->epfd);   //�ر�epoll_create�������ļ�������

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
