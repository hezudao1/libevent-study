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
	struct evepoll *fds;        //evepoll类型的数组,初始大小为32.
	int nfds;                   //表示fds这个数组的元素个数. 初始值为32.
	struct epoll_event *events; //epoll_event类型的数组,提供epoll使用. 初始大小为32, 最大值为4096.
	int nevents;                //表示events数组的大小, 初始值为32.
	int epfd;                   //保存epoll_create函数生成的文件描述符.
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
	if ((epfd = epoll_create(32000)) == -1) {   //创建一个epoll的文件描述符.
		if (errno != ENOSYS)
			event_warn("epoll_create");
		return (NULL);
	}

	FD_CLOSEONEXEC(epfd);                       //设置epfd的closeOnExec属性.

	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;

	/* Initalize fields */                      //分配events的数组.注意类型是epoll_event
	epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		free(epollop);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENTS;
    //分配evepoll的数组.
	epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);
		free(epollop);
		return (NULL);
	}
	epollop->nfds = INITIAL_NFILES;
    //这里要注意了. 初始化了信号处理的东西. 只有这个地方初始化了信号处理的东西.
	evsignal_init(base);

	return (epollop);
}
//这个函数就是负责扩展epollop中的fds数组, 也就是evepoll类型的数组.实际就是对应了libevent的处理.每次增大一倍,最大值是参数max给出的.
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

	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {//epoll系统调用的等待的时间不能太长.最长35分钟
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}
    //这里epoll系统调用就开始wait了. 返回值是-1出错了, 0超时了, 或者 就绪文件描述符的个数. timeout的单位是毫秒.
	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base); //只要epoll_wait函数出错返回了, 就处理信号event插入到active队列中
		return (0);
	} else if (base->sig.evsignal_caught) {
		evsignal_process(base); //只要信号发生了就处理信号.就处理信号event插入到active队列中
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));

	for (i = 0; i < res; i++) { //开始遍历了.
		int what = events[i].events;    //epoll_event结构体中的events字段,表示就绪的事件类型.
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;

		if (fd < 0 || fd >= epollop->nfds)
			continue;
		evep = &epollop->fds[fd];   //从这里可以看出evepoll类型的数组fds和epoll_event类型的数组events的对应关系.可以看出这两个数组的大小并一定是相等, 因为events数组的作用是epoll_wait获得就绪的文件描述符的. 文件描述符不太可能会同一时间全部就绪.
		//这里就可以看出, fds数组的索引就是文件描述符. 通过文件描述符, 两个数组关联.
		if (what & (EPOLLHUP|EPOLLERR)) {   //当一个就绪文件描述符, 对应的文件描述符被挂断,对应的文件描述符出错了.设置读写事件.
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			if (what & EPOLLIN) {//文件描述符是读事件.
				evread = evep->evread;
			}

			if (what & EPOLLOUT) {//文件描述符是写事件.
				evwrite = evep->evwrite;
			}
		}

		if (!(evread||evwrite))
			continue;

		if (evread != NULL) //把对应的read的event结构体,插入到libevent的active队列中.
			event_active(evread, EV_READ, 1);
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);
	}

	if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {    //在这里检查epoll_event类型的数组event是不是不够用了.那就扩展吧.
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
		return (evsignal_add(ev));  //如果是event是信号类型的事件,就会直接插入到信号对应的消息队列中.

	fd = ev->ev_fd;
	if (fd >= epollop->nfds) {
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)   //这里扩展evepoll类型数组fds的大小.因为现在出现的文件描述符超过了fds的大小.
			return (-1);
	}
	evep = &epollop->fds[fd];   //先取出数组中的对应的元素.
	op = EPOLL_CTL_ADD;         //默认的操作是EPOLL_CTL_ADD.
	events = 0;
	if (evep->evread != NULL) { //当evep的读或者写event不为空. 则既要保持原来的监听事件, 也要加上新的事件,所以操作要改成MOD.
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	if (evep->evwrite != NULL) {    //保持原来的监听事件.
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}

	if (ev->ev_events & EV_READ)    //开始保持新的监听事件.
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)   //开始保持新的监听事件.
		events |= EPOLLOUT;

	epev.data.fd = fd;              //保持文件描述符
	epev.events = events;           //保存监听事件.
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)   //开始修改epoll的监听事件.
			return (-1);

	/* Update events responsible */
	if (ev->ev_events & EV_READ)    //这几步就是更新evepoll数组中的元素的信息. 只有epoll_ctl成功之后才更新的.不错.
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
		return (evsignal_del(ev));  //删除对应的信号的信息.从对应信号的事件队列中删除它.

	fd = ev->ev_fd;
	if (fd >= epollop->nfds)        //如果这个event对应的文件描述符, 超过了关联的evepoll的数组的范围.
		return (0);
	evep = &epollop->fds[fd];

	op = EPOLL_CTL_DEL;
	events = 0;

	if (ev->ev_events & EV_READ)    //当event有读事件, 则设置events标志位.
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)   //当event有写事件, 则设置events标志位.
		events |= EPOLLOUT;

	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {  //当event既监听了读又监听了写,那就可以完全删除了.
		if ((events & EPOLLIN) && evep->evwrite != NULL) {      //如果event只监听了读事件, 但是这个event对应的fd, fd关联的evepoll结构中的writeevent 不为空, 那么就走下面语句.
			needwritedelete = 0;    //让epoll监听写事件,不监听读事件. 不删除对应的标志位.
			events = EPOLLOUT;      //更新event的标识位.
			op = EPOLL_CTL_MOD;     //因为之前event已经被放置到epoll的监听队列中.所以, 操作不是EPOLL_CTL_ADD, 而是EPOLL_CTL_MOD. 把读监听改为写监听. 其实这个是一个边缘情况.
		} else if ((events & EPOLLOUT) && evep->evread != NULL) {   //同上面类似. 也是一种边缘清空.
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.fd = fd;
    //这里对应更新对应的evepoll结构体中的数据.
	if (needreaddelete)
		evep->evread = NULL;
	if (needwritedelete)
		evep->evwrite = NULL;
    //
	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)  //调用底层的API了.
		return (-1);

	return (0);
}

static void
epoll_dealloc(struct event_base *base, void *arg)
{
	struct epollop *epollop = arg;

	evsignal_dealloc(base);
	if (epollop->fds)
		free(epollop->fds);     //释放epollop中的evepoll类型的结构体数组.
	if (epollop->events)
		free(epollop->events);  //释放epollop中的epoll_event类型的结构体数组.
	if (epollop->epfd >= 0)
		close(epollop->epfd);   //关闭epoll_create创建的文件描述符

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
