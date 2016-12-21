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

struct event_base *evsignal_base = NULL;        //ȫ�ֱ���,�������event_base����ר����������evsignal_info�е�ev_signal��.

static void evsignal_handler(int sig);          //

/* Callback for when the signal handler write a byte to our signaling socket */
static void
evsignal_cb(int fd, short what, void *arg)      //����������ev_signal�¼�����ʱ�����õĻص�����.
{
	static char signals[1];     //���ǽ���UNIX�׽��ַ��͹�����һ���ֽڵ�.
#ifdef WIN32
	SSIZE_T n;
#else
	ssize_t n;
#endif

	n = recv(fd, signals, sizeof(signals), 0);  //��������ϵͳapi����һ���ֽ�,��������¼�.
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
//�źų�ʼ������,��ʵ��ʼ��һ��base�е�����!!!Ҳ����base�е�sig�ֶ�.���,����Ҫ��evsignal_info��event_base��������.��Ϊ���¾��ǹ������.
int
evsignal_init(struct event_base *base)  //�ú�������epoll_init�Ⱥ����е��õ�.
{
	int i;

	/*
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.����UNIX�׽���,����event_base�е�evsinal_info�ֶε�ev_signal_pair�ֶ�.
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

	FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);//������exec �رյı�ʶλ.
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);
	base->sig.sh_old = NULL;
	base->sig.sh_old_max = 0;                   //���������¼���������źŵı�ŵ����ֵ.
	base->sig.evsignal_caught = 0;              //��ʾ�Ƿ������ź�.
	memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);   //��evsigcaught����,�����������þ��Ǽ�¼ÿ���ź�,�����Ĵ���.
	/* initialize the queues for all events */
	for (i = 0; i < NSIG; ++i)      //������ǳ�ʼ��evsigevents����.������Ԫ�ؾ��Ǳ��淢����ĳ���źŵ�event�¼�,��Щ�¼������γ�����.
		TAILQ_INIT(&base->sig.evsigevents[i]);

        evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);//���÷�����.

	event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],    //������ǳ�ʼ��ev_signal���event�Ľṹ��.���ǻ�û��ע�ᵽevent_base.
		EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);
	base->sig.ev_signal.ev_base = base; //������ǽ�ev_signal���������event_base��.
	base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;//����Ϊʲô��Ҫ���ñ�־λ, �ڻ�û��ע�ᵽevent_base�ϵ�ʱ��.

	return 0;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
int
_evsignal_set_handler(struct event_base *base,
		      int evsignal, void (*handler)(int))//�����������������handler,����ɵ�handler.�µ�handlerʼ�������涨���evsignal_handler����
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
	if (evsignal >= sig->sh_old_max) {  //����һ���ź�,���źŵı�Ŵ���֮ǰ���������е��źŵı��.
		int new_max = evsignal + 1;
		event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
			    __func__, evsignal, sig->sh_old_max));
		p = realloc(sig->sh_old, new_max * sizeof(*sig->sh_old));   //����reallocʹ���±���p����realloc�ķ���ֵ,����������Ϊ�˵�realloc����֮��,�ɵ��ڴ��޷��ͷŵ�bug.
		if (p == NULL) {
			event_warn("realloc");
			return (-1);
		}
        //������ǳ�ʼ���µĲ���.����realloc�����Ǹ���֮ǰ�����ݵ��µ��ڴ���,ͬʱ�Զ��ͷ��ڴ�.
		memset((char *)p + sig->sh_old_max * sizeof(*sig->sh_old),
		    0, (new_max - sig->sh_old_max) * sizeof(*sig->sh_old));

		sig->sh_old_max = new_max;
		sig->sh_old = p;
	}
    //����ʹ�õ��˶���ָ��,�����е������.��ʵ������ȶ�ά�����ʵ��.����ֻ��������ֻ��һ��Ԫ�ص����.��Ϊ������ֻ��һ��Ԫ��,���Ժܹ�ѽ.
	/* allocate space for previous handler out of dynamic array */
	sig->sh_old[evsignal] = malloc(sizeof *sig->sh_old[evsignal]); //���������sh_old������ڴ�. sh_old�����Ԫ���Ǻ���ָ��Ķ���ָ��.����sh_old[evsignal]��
	if (sig->sh_old[evsignal] == NULL) {
		event_warn("malloc");
		return (-1);
	}

	/* save previous handler and setup new handler */
#ifdef HAVE_SIGACTION   //֧��sigaction����ʱ,���ǿ�ʼ��ʼ��sigaction�ṹ��.֮�������µ�handler,ͬʱ���־ɵ�handler.
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
	*sig->sh_old[evsignal] = sh;//������Ǳ����˾ɵ�handler
#endif

	return (0);
}
//���Ƿ�����ĳ���źŵ��¼�event,Ȼ������������Ǹ������event���뵽evsignal_info��ev_singal�ֶζ�Ӧevent������.
int
evsignal_add(struct event *ev)
{//��������ĵ�����������API�е�xxx_add������,����epoll_add.���Ծ����ź���ص�event������eventһ�����뵽insert����.
	int evsignal;//���ǲ�����뵽active���кͶ�ʱ��С����.Ϊʲô�����뵽active������,��Ϊû�б�Ҫ.�źŴ���û�����ȼ��Ĺ���,û��ʱ���ϵ�Ҫ��.
	struct event_base *base = ev->ev_base;//��Ϊ�ź�û�й�ע���ļ�������,���Բ����������뵽����API����
	struct evsignal_info *sig = &ev->ev_base->sig;//�����ź��¼���ʱ��Ҳ��epoll_dispatch�е�epoll_wait���ص�ʱ��,�����κ����, ��ܰ����е��ź��¼�ȫ��������.

	if (ev->ev_events & (EV_READ|EV_WRITE)) //����evsigevents�б����event�����Ƕ�д�¼�.
		event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);
	evsignal = EVENT_SIGNAL(ev);
	assert(evsignal >= 0 && evsignal < NSIG);
	if (TAILQ_EMPTY(&sig->evsigevents[evsignal])) { //�����Ӧ�źŶ���Ϊ��,���˵��Ҫ�����µ�handler,ͬʱ����sh_old[signo]���ڴ�.
		event_debug(("%s: %p: changing signal handler", __func__, ev));
		if (_evsignal_set_handler(
			    base, evsignal, evsignal_handler) == -1) //ע���������������handler,����ɵ�handler.
			return (-1);

		/* catch signals if they happen quickly */
		evsignal_base = base;

		if (!sig->ev_signal_added) {
			if (event_add(&sig->ev_signal, NULL))   //���û�н�ev_signal���ӵ�event_base��ʱ,����ӵ�event_base;
				return (-1);
			sig->ev_signal_added = 1;
		}
	}

	/* multiple events may listen to the same signal */
	TAILQ_INSERT_TAIL(&sig->evsigevents[evsignal], ev, ev_signal_next);//������ǲ���event��������.

	return (0);
}
//�������������ĳ���źŵķ���event�����Ѿ�Ϊ����.���Ծ���Ҫ��ԭ����handler��ԭ��ȥ.
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
//����������Ǹ���ɾ��evsigevents�е�event.
int
evsignal_del(struct event *ev)
{
	struct event_base *base = ev->ev_base;
	struct evsignal_info *sig = &base->sig;
	int evsignal = EVENT_SIGNAL(ev);

	assert(evsignal >= 0 && evsignal < NSIG);

	/* multiple events may listen to the same signal */
	TAILQ_REMOVE(&sig->evsigevents[evsignal], ev, ev_signal_next);//��event�б���,ɾ��ָ����event.

	if (!TAILQ_EMPTY(&sig->evsigevents[evsignal]))  //�����Ϊ��,�Ͳ�������Ĳ�����.
		return (0);

	event_debug(("%s: %p: restoring signal handler", __func__, ev));

	return (_evsignal_restore_handler(ev->ev_base, EVENT_SIGNAL(ev))); //�����Ӧ�Ķ���Ϊ����.�Ǿͻָ�ԭ�����źŴ������.
}
//�������������evsignal_info��sig�źŵ��¼�����Ϊ�������, ���źŵ�event����ʱ,���Ϊÿ���ź����õ��µ��źŴ������.
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
	evsignal_base->sig.evsignal_caught = 1;//�������������evsignal_caught�ĵط�.

#ifndef HAVE_SIGACTION
	signal(sig, evsignal_handler);
#endif

	/* Wake up our notification mechanism */
	send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0); //����һ���ַ�, ��������ev_signal��event�¼�,���Կ�ܾͿ��Կ�ʼ�������о����¼���.
	errno = save_errno;
}
//�ú����������е��źŵĶ����е�����event. Ҳ���ǵ������еĻص�����.
void
evsignal_process(struct event_base *base)
{
	struct evsignal_info *sig = &base->sig;
	struct event *ev, *next_ev;
	sig_atomic_t ncalls;
	int i;

	base->sig.evsignal_caught = 0;
	for (i = 1; i < NSIG; ++i) {    //�������е��źŴ�����������.�������ź�event,�����뵽event_base��active������ȥ.
		ncalls = sig->evsigcaught[i];
		if (ncalls == 0)    //������Ϊi���ź�û�з�����.��ôcontinue��.
			continue;
		sig->evsigcaught[i] -= ncalls;  //�����ֶ�

		for (ev = TAILQ_FIRST(&sig->evsigevents[i]);
		    ev != NULL; ev = next_ev) {
			next_ev = TAILQ_NEXT(ev, ev_signal_next);
			if (!(ev->ev_events & EV_PERSIST))  //����EV_PERSIST����Ҫ��insert����,active����,��ʱ��С��������ɾ����,Ȼ���ټ��뵽����������ȥ.
				event_del(ev);  //������ǳ־��¼�,��ֱ�Ӹɵ����event����.Ȼ��
			event_active(ev, EV_SIGNAL, ncalls);//���ｲevent���뵽active������.ע������źŷ����˶��,��ô��Ӧ���е�event�Ļص�����ȫ����Ҫ������ncalls��.
		}

	}
}
//�������Ǹ�������evsignal_info����.
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
