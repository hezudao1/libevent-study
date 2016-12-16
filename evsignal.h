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

typedef void (*ev_sighandler_t)(int);           //typedefһ������ ���źŵĲ�����.

struct evsignal_info {
	struct event ev_signal;                     //evsignal_infoר����event�ṹ��. �������������ev_signal_pair�е�д�����¼�, ����libevent��event_loop.
	int ev_signal_pair[2];                      //����UNIX���׽��ֵ�������������.
	int ev_signal_added;                        //����ֶα�ʾev_signal���event�Ƿ�ע�ᵽһ��event_base��.
	volatile sig_atomic_t evsignal_caught;      //��ʾ�Ƿ���������һ���ź�.
	struct event_list evsigevents[NSIG];        //NSIG��С��event_list���͵�����. event_list���͵Ķ�����/sys/queue.h��event.h��, ����ʹ�ú궨��. �е㲻̫��.
	sig_atomic_t evsigcaught[NSIG];             //�������Ҳ��NSIG��С,��evsigevents����һһ��Ӧ,��ʾ����ĳ���źŷ����˶��ٴ�.
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;                  //��ϵͳ֧��sigaction����ʱ, ��Ҫʹ��sigaction�ṹ�屣��֮ǰ���źŴ���ʽ. ע��,����sh_old��һ������ָ��,����һ������, �����Ԫ���Ƕ�Ӧ��ĳ���źŵ�֮ǰ���źŴ������.
#else   //�����ʵ��ʹ�õ�����ָ��.���ǲ�̫���, ΪɶҪ�ö���ָ��, ��ʵһ��ָ��͹���,ϵͳһ���ź�ֻҪ����һ��ev_sighandler_t
	ev_sighandler_t **sh_old;                   //����ɵ��źŴ���������Ϊ���ڴ�����ĳ���źŵ������¼�ʱ(evsigevents�е�ĳ��Ԫ��event_list�е�eventȫ����������),��ô��Ҫ��ϵͳ���źŴ������ָ���ԭ����ֵ.
#endif
	int sh_old_max;                             //���ֶξ��Ǽ�¼��ǰ�����������źŵı��. ע���������������źŵı��.������ֱ�Ӿ�����sh_old����Ĵ�С.
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
