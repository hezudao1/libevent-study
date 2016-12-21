/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#include <winsock2.h>
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <assert.h>

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "evutil.h"
#include "log.h"

struct event_base *evsignal_base = NULL;        //全局变量,保存这个event_base就是专门用来处理evsignal_info中的ev_signal的.

static void evsignal_handler(int sig);          //

/* Callback for when the signal handler write a byte to our signaling socket */
static void
evsignal_cb(int fd, short what, void *arg)      //是用来设置ev_signal事件就绪时机调用的回调函数.
{
	static char signals[1];     //就是接受UNIX套接字发送过来的一个字节的.
#ifdef WIN32
	SSIZE_T n;
#else
	ssize_t n;
#endif

	n = recv(fd, signals, sizeof(signals), 0);  //调用网络系统api接受一个字节,处理就绪事件.
	if (n == -1)
		event_err(1, "%s: read", __func__);
}

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif
//信号初始化函数,其实初始化一个base中的数据!!!也就是base中的sig字段.因此,不需要讲evsignal_info与event_base关联起来.因为本事就是关联这的.
int
evsignal_init(struct event_base *base)  //该函数是在epoll_init等函数中调用的.
{
	int i;

	/*
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.创建UNIX套接字,更新event_base中的evsinal_info字段的ev_signal_pair字段.
	 */
	if (evutil_socketpair(
		    AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair) == -1) {
#ifdef WIN32
		/* Make this nonfatal on win32, where sometimes people
		   have localhost firewalled. */
		event_warn("%s: socketpair", __func__);
#else
		event_err(1, "%s: socketpair", __func__);
#endif
		return -1;
	}

	FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);//设置了exec 关闭的标识位.
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);
	base->sig.sh_old = NULL;
	base->sig.sh_old_max = 0;                   //这个参数记录遇到过的信号的编号的最大值.
	base->sig.evsignal_caught = 0;              //表示是否发生过信号.
	memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);   //这evsigcaught数组,这个数组的作用就是记录每种信号,发生的次数.
	/* initialize the queues for all events */
	for (i = 0; i < NSIG; ++i)      //这里就是初始化evsigevents数组.数组中元素就是保存发生了某种信号的event事件,这些事件串联形成链表.
		TAILQ_INIT(&base->sig.evsigevents[i]);

        evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);//设置非阻塞.

	event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],    //这里就是初始化ev_signal这个event的结构体.但是还没有注册到event_base.
		EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);
	base->sig.ev_signal.ev_base = base; //这里就是将ev_signal反向关联到event_base上.
	base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;//这里为什么先要设置标志位, 在还没有注册到event_base上的时候.

	return 0;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
int
_evsignal_set_handler(struct event_base *base,
		      int evsignal, void (*handler)(int))//这个函数就是设置新handler,保存旧的handler.新的handler始终是下面定义的evsignal_handler函数
{
#ifdef HAVE_SIGACTION
	struct sigaction sa;
#else
	ev_sighandler_t sh;
#endif
	struct evsignal_info *sig = &base->sig;
	void *p;

	/*
	 * resize saved signal handler array up to the highest signal number.
	 * a dynamic array is used to keep footprint on the low side.
	 */
	if (evsignal >= sig->sh_old_max) {  //遇到一个信号,该信号的编号大于之前遇到的所有的信号的编号.
		int new_max = evsignal + 1;
		event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
			    __func__, evsignal, sig->sh_old_max));
		p = realloc(sig->sh_old, new_max * sizeof(*sig->sh_old));   //这里realloc使用新变量p保存realloc的返回值,这样做就是为了当realloc出错之后,旧的内存无法释放的bug.
		if (p == NULL) {
			event_warn("realloc");
			return (-1);
		}
        //这里就是初始化新的部分.看来realloc函数是复制之前的数据到新的内存中,同时自动释放内存.
		memset((char *)p + sig->sh_old_max * sizeof(*sig->sh_old),
		    0, (new_max - sig->sh_old_max) * sizeof(*sig->sh_old));

		sig->sh_old_max = new_max;
		sig->sh_old = p;
	}
    //这里使用到了二级指针,所以有点难理解.其实就是类比二维数组的实现.这里只是子数组只有一个元素的情况.因为子数组只有一个元素,所以很怪呀.
	/* allocate space for previous handler out of dynamic array */
	sig->sh_old[evsignal] = malloc(sizeof *sig->sh_old[evsignal]); //在这里分配sh_old数组的内存. sh_old数组的元素是函数指针的二级指针.所以sh_old[evsignal]是
	if (sig->sh_old[evsignal] == NULL) {
		event_warn("malloc");
		return (-1);
	}

	/* save previous handler and setup new handler */
#ifdef HAVE_SIGACTION   //支持sigaction函数时,就是开始初始化sigaction结构体.之后设置新的handler,同时保持旧的handler.
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sa.sa_flags |= SA_RESTART;
	sigfillset(&sa.sa_mask);

	if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
		event_warn("sigaction");
		free(sig->sh_old[evsignal]);
		sig->sh_old[evsignal] = NULL;
		return (-1);
	}
#else
	if ((sh = signal(evsignal, handler)) == SIG_ERR) {
		event_warn("signal");
		free(sig->sh_old[evsignal]);
		sig->sh_old[evsignal] = NULL;
		return (-1);
	}
	*sig->sh_old[evsignal] = sh;//这里就是保存了旧的handler
#endif

	return (0);
}
//就是发生了某种信号的事件event,然后这个函数就是负责将这个event插入到evsignal_info的ev_singal字段对应event队列中.
int
evsignal_add(struct event *ev)
{//这个函数的调用是在网络API中的xxx_add函数中,例如epoll_add.所以就是信号相关的event会与其event一样插入到insert队列.
	int evsignal;//但是不会插入到active队列和定时最小堆中.为什么不加入到active队列中,因为没有必要.信号处理没有优先级的功能,没有时间上的要求.
	struct event_base *base = ev->ev_base;//因为信号没有关注的文件描述符,所以不会真正加入到网络API监听
	struct evsignal_info *sig = &ev->ev_base->sig;//处理信号事件的时机也是epoll_dispatch中的epoll_wait返回的时候,不管任何情况, 框架吧所有的信号事件全部处理了.

	if (ev->ev_events & (EV_READ|EV_WRITE)) //看来evsigevents中保存的event不能是对写事件.
		event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);
	evsignal = EVENT_SIGNAL(ev);
	assert(evsignal >= 0 && evsignal < NSIG);
	if (TAILQ_EMPTY(&sig->evsigevents[evsignal])) { //如果对应信号队列为空,则就说明要设置新的handler,同时分配sh_old[signo]的内存.
		event_debug(("%s: %p: changing signal handler", __func__, ev));
		if (_evsignal_set_handler(
			    base, evsignal, evsignal_handler) == -1) //注意这里就是设置新handler,保存旧的handler.
			return (-1);

		/* catch signals if they happen quickly */
		evsignal_base = base;

		if (!sig->ev_signal_added) {
			if (event_add(&sig->ev_signal, NULL))   //如果没有将ev_signal增加到event_base上时,就添加到event_base;
				return (-1);
			sig->ev_signal_added = 1;
		}
	}

	/* multiple events may listen to the same signal */
	TAILQ_INSERT_TAIL(&sig->evsigevents[evsignal], ev, ev_signal_next);//这里就是插入event到队列中.

	return (0);
}
//这个函数发生在某个信号的发生event队列已经为空了.所以就需要将原来的handler还原回去.
int
_evsignal_restore_handler(struct event_base *base, int evsignal)
{
	int ret = 0;
	struct evsignal_info *sig = &base->sig;
#ifdef HAVE_SIGACTION
	struct sigaction *sh;
#else
	ev_sighandler_t *sh;
#endif

	/* restore previous handler */
	sh = sig->sh_old[evsignal];
	sig->sh_old[evsignal] = NULL;
#ifdef HAVE_SIGACTION
	if (sigaction(evsignal, sh, NULL) == -1) {
		event_warn("sigaction");
		ret = -1;
	}
#else
	if (signal(evsignal, *sh) == SIG_ERR) {
		event_warn("signal");
		ret = -1;
	}
#endif
	free(sh);

	return ret;
}
//这个函数就是负责删除evsigevents中的event.
int
evsignal_del(struct event *ev)
{
	struct event_base *base = ev->ev_base;
	struct evsignal_info *sig = &base->sig;
	int evsignal = EVENT_SIGNAL(ev);

	assert(evsignal >= 0 && evsignal < NSIG);

	/* multiple events may listen to the same signal */
	TAILQ_REMOVE(&sig->evsigevents[evsignal], ev, ev_signal_next);//在event列表中,删除指定的event.

	if (!TAILQ_EMPTY(&sig->evsigevents[evsignal]))  //如果不为空,就不走下面的步骤了.
		return (0);

	event_debug(("%s: %p: restoring signal handler", __func__, ev));

	return (_evsignal_restore_handler(ev->ev_base, EVENT_SIGNAL(ev))); //如果对应的队列为空了.那就恢复原来的信号处理程序.
}
//这个函数就是在evsignal_info中sig信号的事件队列为空情况下, 该信号的event发生时,框架为每个信号设置的新的信号处理程序.
static void
evsignal_handler(int sig)
{
	int save_errno = errno;

	if (evsignal_base == NULL) {
		event_warn(
			"%s: received signal %d, but have no base configured",
			__func__, sig);
		return;
	}

	evsignal_base->sig.evsigcaught[sig]++;
	evsignal_base->sig.evsignal_caught = 1;//这里就是设置了evsignal_caught的地方.

#ifndef HAVE_SIGACTION
	signal(sig, evsignal_handler);
#endif

	/* Wake up our notification mechanism */
	send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0); //发送一个字符, 用来唤醒ev_signal的event事件,所以框架就可以开始处理所有就绪事件了.
	errno = save_errno;
}
//该函数处理所有的信号的队列中的所有event. 也就是调用所有的回调函数.
void
evsignal_process(struct event_base *base)
{
	struct evsignal_info *sig = &base->sig;
	struct event *ev, *next_ev;
	sig_atomic_t ncalls;
	int i;

	base->sig.evsignal_caught = 0;
	for (i = 1; i < NSIG; ++i) {    //遍历所有的信号处理程序的链表.将所有信号event,都加入到event_base的active队列中去.
		ncalls = sig->evsigcaught[i];
		if (ncalls == 0)    //如果编号为i的信号没有发生了.那么continue了.
			continue;
		sig->evsigcaught[i] -= ncalls;  //更新字段

		for (ev = TAILQ_FIRST(&sig->evsigevents[i]);
		    ev != NULL; ev = next_ev) {
			next_ev = TAILQ_NEXT(ev, ev_signal_next);
			if (!(ev->ev_events & EV_PERSIST))  //不是EV_PERSIST就先要在insert队列,active队列,定时最小堆上面先删除它,然后再加入到就绪队列中去.
				event_del(ev);  //如果不是持久事件,就直接干掉这个event对象.然后
			event_active(ev, EV_SIGNAL, ncalls);//这里讲event插入到active队列中.注意这个信号发生了多次,那么对应所有的event的回调函数全部都要被调用ncalls次.
		}

	}
}
//函数就是负责销毁evsignal_info对象.
void
evsignal_dealloc(struct event_base *base)
{
	int i = 0;
	if (base->sig.ev_signal_added) {
		event_del(&base->sig.ev_signal);
		base->sig.ev_signal_added = 0;
	}
	for (i = 0; i < NSIG; ++i) {
		if (i < base->sig.sh_old_max && base->sig.sh_old[i] != NULL)
			_evsignal_restore_handler(base, i);
	}

	if (base->sig.ev_signal_pair[0] != -1) {
		EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[0]);
		base->sig.ev_signal_pair[0] = -1;
	}
	if (base->sig.ev_signal_pair[1] != -1) {
		EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[1]);
		base->sig.ev_signal_pair[1] = -1;
	}
	base->sig.sh_old_max = 0;

	/* per index frees are handled in evsig_del() */
	if (base->sig.sh_old) {
		free(base->sig.sh_old);
		base->sig.sh_old = NULL;
	}
}
