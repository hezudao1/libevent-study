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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);           //typedef一个类型 是信号的操作符.

struct evsignal_info {
	struct event ev_signal;                     //evsignal_info专属的event结构体. 用来监听下面的ev_signal_pair中的写就绪事件, 唤醒libevent的event_loop.
	int ev_signal_pair[2];                      //保存UNIX的套接字的描述符的数组.
	int ev_signal_added;                        //这个字段表示ev_signal这个event是否被注册到一个event_base中.
	volatile sig_atomic_t evsignal_caught;      //表示是否发生了任意一种信号.
	struct event_list evsigevents[NSIG];        //NSIG大小的event_list类型的数组. event_list类型的定义在/sys/queue.h和event.h中, 都是使用宏定义. 有点不太懂.
	sig_atomic_t evsigcaught[NSIG];             //这个数组也是NSIG大小,与evsigevents数组一一对应,表示的是某个信号发生了多少次.
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;                  //当系统支持sigaction函数时, 就要使用sigaction结构体保存之前的信号处理方式. 注意,这里sh_old是一个二级指针,它是一个数组, 数组的元素是对应于某个信号的之前的信号处理程序.
#else   //这里的实现使用到二级指针.就是不太理解, 为啥要用二级指针, 其实一级指针就够了,系统一个信号只要保存一个ev_sighandler_t
	ev_sighandler_t **sh_old;                   //保存旧的信号处理程序就是为了在处理完某个信号的所有事件时(evsigevents中的某个元素event_list中的event全部被处理了),那么就要本系统的信号处理程序恢复到原来的值.
#endif
	int sh_old_max;                             //这字段就是记录当前遇到的最大的信号的编号. 注意遇到过的最大的信号的编号.这个编号直接决定了sh_old数组的大小.
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
