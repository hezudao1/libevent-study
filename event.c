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
	gettime(base, &base->event_tv);//更新event_base的时间

	min_heap_ctor(&base->timeheap); //初始化堆, 但是没有分配内存.
	TAILQ_INIT(&base->eventqueue);  //初始化insert队列.
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;

	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) {
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base); //当创建一个event_base时,就会调用每个网络调用的init函数.
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD"))
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);//初始化active队列

	return (base);
}

void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)           //当base就是current_base, 那么就要将全局变量current_base先设置为null.这样后面只需要处理base这个变量了.
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) { //删除insert队列中的event
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);  //注意这里,删除关于event的所有记录.包括各个队列中的event的副本,和网络API监听的对应部分.
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {  //这里min_heap_top返回的是定时堆的根节点. 因为这里的堆是数组实现的.根节点的地址就是这个数组的首地址, 所以直接析构根节点就是释放了这个堆了.
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {             //这里要对active队列(优先队列)中的event,全部析构.两层循环.
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
		assert(TAILQ_EMPTY(base->activequeues[i])); //从这里可以看出上面dealloc函数并不负责释放active队列的内存.

	assert(min_heap_empty(&base->timeheap));//判断base的定时最小堆中,没有event的时间了. 不然就会报错了.
	min_heap_dtor(&base->timeheap);//释放最小堆的内存. 讲一下,这里的堆使用数组实现的,数组中元素是event*类型变量.

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]); //从这里可以看出上面dealloc函数并不负责释放active队列的内存.这里就是释放各个优先级的队列.
	free(base->activequeues);//因为优先队列首先是一个数组, 数组的元素是一个链表, 所以最后要释放数组.

	assert(TAILQ_EMPTY(&base->eventqueue));//最后在判读一次,非空报错了

	free(base);
}

/* reinitialized the event base after a fork *///重新初始化
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
	if (base->sig.ev_signal_added) {    //如果该event_base对象已经设置了信号处理程序.那么就要清除信号signal_ev事件.
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,      //这里将在inserted队列中的信号处理部分的那个专属event删除, 也就是evsignal_info的ev_signal字段.
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)   //在active队列中删除指定event.
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;                      //设置标志位,表示event_base中的sigevent_info的ev_signal的event, 没有注册到event_base上.
	}

	if (base->evsel->dealloc != NULL)   //调用dealloc.这里调用的底层网络API的销毁函数.例如在epoll_dealloc函数中, 就是销毁epollop结构体的所有资源.
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);//再初始化它.
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {//这里又把原来base中队列中的event,重新加回来. 在这里看来evsel->dealloc和evsel->init并没有改变base中的event队列的数据.
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}
//初始化base的active队列. 主要是生成active队里的内存部分.
int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}
//初始化base的active队列. 主要是生成active队里的内存部分.
int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;

	if (base->event_count_active)           //当event_base中还有active的event时.
		return (-1);

	if (npriorities == base->nactivequeues)
		return (0);

	if (base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]);        //销毁优先队列active中各个队列.
		}
		free(base->activequeues);       //销毁优先队列active.
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
//判断这个event_base中还有没有event事件
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

	for (i = 0; i < base->nactivequeues; ++i) { //这样的处理, 会导致低优先级的事件很难获得运行的机会.
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {    //从上面的遍历结果中得到一个某优先级的子序列, 遍历.
		if (ev->ev_events & EV_PERSIST) //当event是持久的那种, 每次执行就从active队列中删除.
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);  //如果不是持久的那种, 直接将event彻底干掉.

		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) { //允许多次调用回调函数!!!
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
//这个函数就是在下面event_base_loopexit中, 注册一次性event时使用的回调函数.
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
//这个函数高级呀, 使用自己的定时机制暂停自己的loop.
int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,  //设置一个一次性的event就是为了暂停自己
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}
//函数就是设置event_break字段, 会将event_loop的循环打破.
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
//关键的函数, 循环:校对event_base对象和定时任务最小堆上所有event的时间, 记录调用网络api的时间, 调用网络api, 记录调用之后的时间, 检查定时堆并将到期的event插入active队列
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

	if (base->sig.ev_signal_added)  //当event_base的evsignal_info的ev_signal已经添加到了event_base中
		evsignal_base = base;   //则更新evsignal_base等于base;
	done = 0;
	while (!done) { //当event_base对象是一次运行的那种情况, 就会使用到done变量, 进行终止loop操作.
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {// 事件主循环 查看是否需要跳出循环 程序可以调用event_loopexit_cb()设置event_gotterm标记 调用event_base_loopbreak()设置event_base标记
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

		timeout_correct(base, &tv); //这里就是用当前的绝对时间更新event_base的event_tv字段和参数tv中.

		tv_p = &tv;
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) { //如果base管理的active的event还有, 并且flags是阻塞的.
			timeout_next(base, &tv_p);  //则将tv_p的值设置为小根堆的根节点的时间(也就是最近一次定时event的绝对时间).
		} else {
			/*
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);//清空tv之前保存的当前时间. 目前的状态是:base的active队列没有事件了 或者 flags参数显示非阻塞状态.
		}

		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {  //如果这个base的所有的event事件都没有了!!!那么就返回吧.注意上面的只是active中的event没有了的情况.
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* update last old time */
		gettime(base, &base->event_tv);//获取当前时间, 更新event_tv属性.

		/* clear time cache */
		base->tv_cache.tv_sec = 0;//清空时间的缓存.
        //下面的函数调用将所有的就绪IO相关的event, 全部插入到active队列中.
		res = evsel->dispatch(base, evbase, tv_p);  //调动真正的select epoll poll等函数的时候.这个语句返回的时候, 就是表明有事件已经就绪了, 或者已经经过了tv_p的时间段.

		if (res == -1)  //这些底层网络api调用失败就返回了.
			return (-1);
		gettime(base, &base->tv_cache); //现在设置时间的缓存, 为当前时间. 从这里的代码可以看出, base->event_tv保持的是调用网络api之前的时间, base->cache保存的是调用网络api之后的时间.
        //这个函数调用就是将所有的定时任务的event插入到active队列中去.
		timeout_process(base);  //这个函数就是遍历定时任务堆,将到期的event插入到active列表中.
        //下面就开始执行active队列中的event对应的回调函数了.就是处理就绪事件了.
		if (base->event_count_active) { //如果active队列中有event
			event_process_active(base); //这里就开始遍历active队列了. 根据active中的event优先级进行调用注册的回调函数. 所以调用定时任务的时机是, 网络api返回的时候
			if (!base->event_count_active && (flags & EVLOOP_ONCE)) //这里再次判断, 是不是一次运行的, active中有没有event事件,函数返回.
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)//这里再次判断, 是不是一次运行的, active中有没有event事件,函数返回.
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;//清空保持上一次调用完网络api的时间.

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */
//一个本地使用结构体.
struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */
//这个函数是本地的静态函数, 这函数就是为了让框架在一次运行事件激活时, 调用这个函数, 这个函数内部在调动设置event时设置的回调函数, 最后这个event_once_cb函数释放内存.
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
	return event_base_once(current_base, fd, events, callback, arg, tv);//因为current_base是全局变量, 可能有多个线程共用, 所以会有竞争问题.
}

/* Schedules an event once *///设置一次运行事件, 当tv为null的时候马上Active. callback是回调函数.
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */ //不支持信号一次发生.
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;
    //只能处理简单的定时任务或者读写event
	if (events == EV_TIMEOUT) { //单纯的定时event.
		if (tv == NULL) {
			evutil_timerclear(&etv); //清空etv的字段,这里表示事件立即生效!
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce); //注意event_once_cb是一个函数!这里既是调用下方的event_set函数,初始化event事件. 将event与current_base关联,
	} else if (events & (EV_READ|EV_WRITE)) { //io事件
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */ //其他事件就错误了.
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev); //将event关联到base上.
	if (res == 0)
		res = event_add(&eonce->ev, tv);//将event注册到这个base上, 如果tv参数为null, 应该马上到期, 进入Active队列中.
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}
//将一个事先分配好内存的event对象, 进行初始化, 并且将event对象与current_base对象关联起来.注意不是安装到current_base上.
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
//将event事件连接到一个event_base对象,注意这个不是将这个event事件安装到了event_base,只是将event的对应字段进行修改了.
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
//设置一个event事件的优先级.当event处于active队列中, 就不能修改它的优先级.
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
//函数就是拿一个参数event, 与当前event事件进行比较.得出一个event的特征.包括是否插入到insert队列,active队列,超时最小堆, 并且包括event是否关注timeout,read,write,signal四种事件.
int
event_pending(struct event *ev, short event, struct timeval *tv) //如果event关注了Timeout事件时,tv参数返回到期的时间,这个时间是绝对时间.
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
//函数就是根据event的不同插入不同的队列或者定时堆中.
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
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {//判断tv是不是为null, 当为null, 表示是一个定时任务. 同时判断这个event没有加入到定时event的小根堆中.
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1) //这里就是要添加一个定时任务, 这里先在小跟堆中预留一个位置.防止之后没有位置.
			return (-1);  /* ENOMEM == errno */
	}

	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {    //如果event是io事件或者是signal事件.并且没有加入到insert队列和active队里, 则直接将event添加到队列中.
		res = evsel->add(evbase, ev);
		if (res != -1)
			event_queue_insert(base, ev, EVLIST_INSERTED);
	}

	/*
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	if (res != -1 && tv != NULL) { //这里表示, event也是一个定时event,所以就要插入到定时小根堆中.
		struct timeval now;

		/*
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);   //如果event已经在定时堆中, 那就先删除掉.

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {    //如果event在到期了,并且在active队列中,则还要清理event其他属性.
			/* See if we are just active executing this
			 * event in a loop
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}

			event_queue_remove(base, ev, EVLIST_ACTIVE); //从active队列中,删除这个event事件.
		}

		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout); //这里就是将event的到期时间设置为绝对时间.

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT); //插入到定时event的堆中.
	}

	return (res);
}
//这个函数就是将一个event从insert,signal,active三个队列中进行删除, 最后调用base中专属eventtop的del函数销毁event. 比较简单.注意remove函数就是从某个队列中删除event,这里del是销毁event
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
//函数就是简单的将event的状态改变成active状态!,就是修改标志属性.然后插入到Active队列中.
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
//函数的作用就是,检查小根堆的根节点是否到期,没到期就返回到期的时间长度.
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	if ((ev = min_heap_top(&base->timeheap)) == NULL) { //min_heap_top函数就是获取小根堆的根节点.
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	if (gettime(base, &now) == -1)
		return (-1);

	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) { //如果根节点的时间小于当前时间, 说明这个小根堆中所有event都到期了.找不到下一个即将到期的event.
		evutil_timerclear(tv);
		return (0);
	}

	evutil_timersub(&ev->ev_timeout, &now, tv); //如果根节点的时间大于now,就计算差值, 保存到tv_p的内存中.

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
//函数就是更新event_base中的时间字段event_tv, 当出现event_tv的时间大于当前时间时, 就对小根堆中所有的time_event进行修正.
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
	if (evutil_timercmp(tv, &base->event_tv, >=)) {//检查当前时间, 大于base保存的时间.正常情况下就是这样的.然后更新base的时间. 当小于的情况出现了, 是异常情况.
		base->event_tv = *tv;
		return;
	}
    //异常情况下,要计算出时间的差值, 对小根堆中的event进行全部修正.
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
//这个函数就是对base,进行检查小根堆的时间event, 只要找到到期的event, 然后,在所有队列中删除, 最后插入到Active队列中.
void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;

	if (min_heap_empty(&base->timeheap))
		return;

	gettime(base, &now); //得到当前时间

	while ((ev = min_heap_top(&base->timeheap))) { //从base中的时间的小根堆中,找到一个event, 这个event的时间已经小于now当前时间,说明了这个event已经到期, 要插入到Active队列中.
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);//然后从对应的队列中删除这个event. 注意这里删除定时最小堆中的元素, 最小堆会自动调整保持顺序.

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		event_active(ev, EV_TIMEOUT, 1);//更新event的属性,插入到Active队列中去.
	}
}
//从event_base中某个队列中删除某个event
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
//函数很简单, 就是根据参数queue的不同, 插入到event_base的队列中.event_base队列有IO,Signal,Active三种队列.
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
//获取当前current_base中的evsel的name属性, 这里的意思就是event_base的event_top字段中的名字属性.
const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
