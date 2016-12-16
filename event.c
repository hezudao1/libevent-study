/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
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

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}

static int
gettime(struct event_base *base, struct timeval *tp)
{
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif

	return (evutil_gettimeofday(tp, NULL));
}

struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;

	return (base);
}

struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;

	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);

	event_sigcb = NULL;
	event_gotsig = 0;

	detect_monotonic();
	gettime(base, &base->event_tv);//����event_base��ʱ��

	min_heap_ctor(&base->timeheap); //��ʼ����, ����û�з����ڴ�.
	TAILQ_INIT(&base->eventqueue);  //��ʼ��insert����.
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;

	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) {
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base); //������һ��event_baseʱ,�ͻ����ÿ��������õ�init����.
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD"))
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);//��ʼ��active����

	return (base);
}

void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)           //��base����current_base, ��ô��Ҫ��ȫ�ֱ���current_base������Ϊnull.��������ֻ��Ҫ����base���������.
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) { //ɾ��insert�����е�event
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);  //ע������,ɾ������event�����м�¼.�������������е�event�ĸ���,������API�����Ķ�Ӧ����.
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {  //����min_heap_top���ص��Ƕ�ʱ�ѵĸ��ڵ�. ��Ϊ����Ķ�������ʵ�ֵ�.���ڵ�ĵ�ַ�������������׵�ַ, ����ֱ���������ڵ�����ͷ����������.
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {             //����Ҫ��active����(���ȶ���)�е�event,ȫ������.����ѭ��.
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i])); //��������Կ�������dealloc�������������ͷ�active���е��ڴ�.

	assert(min_heap_empty(&base->timeheap));//�ж�base�Ķ�ʱ��С����,û��event��ʱ����. ��Ȼ�ͻᱨ����.
	min_heap_dtor(&base->timeheap);//�ͷ���С�ѵ��ڴ�. ��һ��,����Ķ�ʹ������ʵ�ֵ�,������Ԫ����event*���ͱ���.

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]); //��������Կ�������dealloc�������������ͷ�active���е��ڴ�.��������ͷŸ������ȼ��Ķ���.
	free(base->activequeues);//��Ϊ���ȶ���������һ������, �����Ԫ����һ������, �������Ҫ�ͷ�����.

	assert(TAILQ_EMPTY(&base->eventqueue));//������ж�һ��,�ǿձ�����

	free(base);
}

/* reinitialized the event base after a fork *///���³�ʼ��
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {    //�����event_base�����Ѿ��������źŴ������.��ô��Ҫ����ź�signal_ev�¼�.
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,      //���ｫ��inserted�����е��źŴ����ֵ��Ǹ�ר��eventɾ��, Ҳ����evsignal_info��ev_signal�ֶ�.
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)   //��active������ɾ��ָ��event.
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;                      //���ñ�־λ,��ʾevent_base�е�sigevent_info��ev_signal��event, û��ע�ᵽevent_base��.
	}

	if (base->evsel->dealloc != NULL)   //����dealloc.������õĵײ�����API�����ٺ���.������epoll_dealloc������, ��������epollop�ṹ���������Դ.
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);//�ٳ�ʼ����.
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {//�����ְ�ԭ��base�ж����е�event,���¼ӻ���. �����￴��evsel->dealloc��evsel->init��û�иı�base�е�event���е�����.
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}
//��ʼ��base��active����. ��Ҫ������active������ڴ沿��.
int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}
//��ʼ��base��active����. ��Ҫ������active������ڴ沿��.
int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;

	if (base->event_count_active)           //��event_base�л���active��eventʱ.
		return (-1);

	if (npriorities == base->nactivequeues)
		return (0);

	if (base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);        //�������ȶ���active�и�������.
		}
		free(base->activequeues);       //�������ȶ���active.
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}
//�ж����event_base�л���û��event�¼�
int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	for (i = 0; i < base->nactivequeues; ++i) { //�����Ĵ���, �ᵼ�µ����ȼ����¼����ѻ�����еĻ���.
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {    //������ı�������еõ�һ��ĳ���ȼ���������, ����.
		if (ev->ev_events & EV_PERSIST) //��event�ǳ־õ�����, ÿ��ִ�оʹ�active������ɾ��.
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);  //������ǳ־õ�����, ֱ�ӽ�event���׸ɵ�.

		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) { //�����ε��ûص�����!!!
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (event_gotsig || base->event_break)
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}
//�����������������event_base_loopexit��, ע��һ����eventʱʹ�õĻص�����.
static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}
//��������߼�ѽ, ʹ���Լ��Ķ�ʱ������ͣ�Լ���loop.
int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,  //����һ��һ���Ե�event����Ϊ����ͣ�Լ�
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}
//������������event_break�ֶ�, �Ὣevent_loop��ѭ������.
int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}
//�ؼ��ĺ���, ѭ��:У��event_base����Ͷ�ʱ������С��������event��ʱ��, ��¼��������api��ʱ��, ��������api, ��¼����֮���ʱ��, ��鶨ʱ�Ѳ������ڵ�event����active����
int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	if (base->sig.ev_signal_added)  //��event_base��evsignal_info��ev_signal�Ѿ���ӵ���event_base��
		evsignal_base = base;   //�����evsignal_base����base;
	done = 0;
	while (!done) { //��event_base������һ�����е��������, �ͻ�ʹ�õ�done����, ������ֹloop����.
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {// �¼���ѭ�� �鿴�Ƿ���Ҫ����ѭ�� ������Ե���event_loopexit_cb()����event_gotterm��� ����event_base_loopbreak()����event_base���
			base->event_gotterm = 0;
			break;
		}

		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		/* You cannot use this interface for multi-threaded apps */
		while (event_gotsig) {
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}

		timeout_correct(base, &tv); //��������õ�ǰ�ľ���ʱ�����event_base��event_tv�ֶκͲ���tv��.

		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) { //���base�����active��event����, ����flags��������.
			timeout_next(base, &tv_p);  //��tv_p��ֵ����ΪС���ѵĸ��ڵ��ʱ��(Ҳ�������һ�ζ�ʱevent�ľ���ʱ��).
		} else {
			/*
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);//���tv֮ǰ����ĵ�ǰʱ��. Ŀǰ��״̬��:base��active����û���¼��� ���� flags������ʾ������״̬.
		}

		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {  //������base�����е�event�¼���û����!!!��ô�ͷ��ذ�.ע�������ֻ��active�е�eventû���˵����.
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* update last old time */
		gettime(base, &base->event_tv);//��ȡ��ǰʱ��, ����event_tv����.

		/* clear time cache */
		base->tv_cache.tv_sec = 0;//���ʱ��Ļ���.
        //����ĺ������ý����еľ���IO��ص�event, ȫ�����뵽active������.
		res = evsel->dispatch(base, evbase, tv_p);  //����������select epoll poll�Ⱥ�����ʱ��.�����䷵�ص�ʱ��, ���Ǳ������¼��Ѿ�������, �����Ѿ�������tv_p��ʱ���.

		if (res == -1)  //��Щ�ײ�����api����ʧ�ܾͷ�����.
			return (-1);
		gettime(base, &base->tv_cache); //��������ʱ��Ļ���, Ϊ��ǰʱ��. ������Ĵ�����Կ���, base->event_tv���ֵ��ǵ�������api֮ǰ��ʱ��, base->cache������ǵ�������api֮���ʱ��.
        //����������þ��ǽ����еĶ�ʱ�����event���뵽active������ȥ.
		timeout_process(base);  //����������Ǳ�����ʱ�����,�����ڵ�event���뵽active�б���.
        //����Ϳ�ʼִ��active�����е�event��Ӧ�Ļص�������.���Ǵ�������¼���.
		if (base->event_count_active) { //���active��������event
			event_process_active(base); //����Ϳ�ʼ����active������. ����active�е�event���ȼ����е���ע��Ļص�����. ���Ե��ö�ʱ�����ʱ����, ����api���ص�ʱ��
			if (!base->event_count_active && (flags & EVLOOP_ONCE)) //�����ٴ��ж�, �ǲ���һ�����е�, active����û��event�¼�,��������.
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)//�����ٴ��ж�, �ǲ���һ�����е�, active����û��event�¼�,��������.
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;//��ձ�����һ�ε���������api��ʱ��.

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */
//һ������ʹ�ýṹ��.
struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */
//��������Ǳ��صľ�̬����, �⺯������Ϊ���ÿ����һ�������¼�����ʱ, �����������, ��������ڲ��ڵ�������eventʱ���õĻص�����, ������event_once_cb�����ͷ��ڴ�.
static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);//��Ϊcurrent_base��ȫ�ֱ���, �����ж���̹߳���, ���Ի��о�������.
}

/* Schedules an event once *///����һ�������¼�, ��tvΪnull��ʱ������Active. callback�ǻص�����.
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */ //��֧���ź�һ�η���.
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;
    //ֻ�ܴ���򵥵Ķ�ʱ������߶�дevent
	if (events == EV_TIMEOUT) { //�����Ķ�ʱevent.
		if (tv == NULL) {
			evutil_timerclear(&etv); //���etv���ֶ�,�����ʾ�¼�������Ч!
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce); //ע��event_once_cb��һ������!������ǵ����·���event_set����,��ʼ��event�¼�. ��event��current_base����,
	} else if (events & (EV_READ|EV_WRITE)) { //io�¼�
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */ //�����¼��ʹ�����.
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev); //��event������base��.
	if (res == 0)
		res = event_add(&eonce->ev, tv);//��eventע�ᵽ���base��, ���tv����Ϊnull, Ӧ�����ϵ���, ����Active������.
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}
//��һ�����ȷ�����ڴ��event����, ���г�ʼ��, ���ҽ�event������current_base�����������.ע�ⲻ�ǰ�װ��current_base��.
void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{
	/* Take the current base - caller needs to set the real base later */
	ev->ev_base = current_base;

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	min_heap_elem_init(ev);

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;
}
//��event�¼����ӵ�һ��event_base����,ע��������ǽ����event�¼���װ����event_base,ֻ�ǽ�event�Ķ�Ӧ�ֶν����޸���.
int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */
//����һ��event�¼������ȼ�.��event����active������, �Ͳ����޸��������ȼ�.
int
event_priority_set(struct event *ev, int pri)
{
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */
//����������һ������event, �뵱ǰevent�¼����бȽ�.�ó�һ��event������.�����Ƿ���뵽insert����,active����,��ʱ��С��, ���Ұ���event�Ƿ��עtimeout,read,write,signal�����¼�.
int
event_pending(struct event *ev, short event, struct timeval *tv) //���event��ע��Timeout�¼�ʱ,tv�������ص��ڵ�ʱ��,���ʱ���Ǿ���ʱ��.
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}
//�������Ǹ���event�Ĳ�ͬ���벻ͬ�Ķ��л��߶�ʱ����.
int
event_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *base = ev->ev_base;
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {//�ж�tv�ǲ���Ϊnull, ��Ϊnull, ��ʾ��һ����ʱ����. ͬʱ�ж����eventû�м��뵽��ʱevent��С������.
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1) //�������Ҫ���һ����ʱ����, ��������С������Ԥ��һ��λ��.��ֹ֮��û��λ��.
			return (-1);  /* ENOMEM == errno */
	}

	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {    //���event��io�¼�������signal�¼�.����û�м��뵽insert���к�active����, ��ֱ�ӽ�event��ӵ�������.
		res = evsel->add(evbase, ev);
		if (res != -1)
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}

	/*
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	if (res != -1 && tv != NULL) { //�����ʾ, eventҲ��һ����ʱevent,���Ծ�Ҫ���뵽��ʱС������.
		struct timeval now;

		/*
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);   //���event�Ѿ��ڶ�ʱ����, �Ǿ���ɾ����.

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {    //���event�ڵ�����,������active������,��Ҫ����event��������.
			/* See if we are just active executing this
			 * event in a loop
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}

			event_queue_remove(base, ev, EVLIST_ACTIVE); //��active������,ɾ�����event�¼�.
		}

		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout); //������ǽ�event�ĵ���ʱ������Ϊ����ʱ��.

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT); //���뵽��ʱevent�Ķ���.
	}

	return (res);
}
//����������ǽ�һ��event��insert,signal,active���������н���ɾ��, ������base��ר��eventtop��del��������event. �Ƚϼ�.ע��remove�������Ǵ�ĳ��������ɾ��event,����del������event
int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));
	}

	return (0);
}
//�������Ǽ򵥵Ľ�event��״̬�ı��active״̬!,�����޸ı�־����.Ȼ����뵽Active������.
void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}
//���������þ���,���С���ѵĸ��ڵ��Ƿ���,û���ھͷ��ص��ڵ�ʱ�䳤��.
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	if ((ev = min_heap_top(&base->timeheap)) == NULL) { //min_heap_top�������ǻ�ȡС���ѵĸ��ڵ�.
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	if (gettime(base, &now) == -1)
		return (-1);

	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) { //������ڵ��ʱ��С�ڵ�ǰʱ��, ˵�����С����������event��������.�Ҳ�����һ���������ڵ�event.
		evutil_timerclear(tv);
		return (0);
	}

	evutil_timersub(&ev->ev_timeout, &now, tv); //������ڵ��ʱ�����now,�ͼ����ֵ, ���浽tv_p���ڴ���.

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */
//�������Ǹ���event_base�е�ʱ���ֶ�event_tv, ������event_tv��ʱ����ڵ�ǰʱ��ʱ, �Ͷ�С���������е�time_event��������.
static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)
		return;

	/* Check if time is running backwards */
	gettime(base, tv);
	if (evutil_timercmp(tv, &base->event_tv, >=)) {//��鵱ǰʱ��, ����base�����ʱ��.��������¾���������.Ȼ�����base��ʱ��. ��С�ڵ����������, ���쳣���.
		base->event_tv = *tv;
		return;
	}
    //�쳣�����,Ҫ�����ʱ��Ĳ�ֵ, ��С�����е�event����ȫ������.
	event_debug(("%s: time is running backwards, corrected",
		    __func__));
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;
}
//����������Ƕ�base,���м��С���ѵ�ʱ��event, ֻҪ�ҵ����ڵ�event, Ȼ��,�����ж�����ɾ��, �����뵽Active������.
void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))
		return;

	gettime(base, &now); //�õ���ǰʱ��

	while ((ev = min_heap_top(&base->timeheap))) { //��base�е�ʱ���С������,�ҵ�һ��event, ���event��ʱ���Ѿ�С��now��ǰʱ��,˵�������event�Ѿ�����, Ҫ���뵽Active������.
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);//Ȼ��Ӷ�Ӧ�Ķ�����ɾ�����event. ע������ɾ����ʱ��С���е�Ԫ��, ��С�ѻ��Զ���������˳��.

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);//����event������,���뵽Active������ȥ.
	}
}
//��event_base��ĳ��������ɾ��ĳ��event
void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--;

	ev->ev_flags &= ~queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active--;
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_TIMEOUT:
		min_heap_erase(&base->timeheap, ev);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}
//�����ܼ�, ���Ǹ��ݲ���queue�Ĳ�ͬ, ���뵽event_base�Ķ�����.event_base������IO,Signal,Active���ֶ���.
void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	ev->ev_flags |= queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_TIMEOUT: {
		min_heap_push(&base->timeheap, ev);
		break;
	}
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/*
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */
//��ȡ��ǰcurrent_base�е�evsel��name����, �������˼����event_base��event_top�ֶ��е���������.
const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
