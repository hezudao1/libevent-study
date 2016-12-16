/*
 * Copyright (c) 2002-2004 Niels Provos <provos@citi.umich.edu>
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

#include <sys/types.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef WIN32
#include <winsock2.h>
#endif

#include "evutil.h"
#include "event.h"

/* prototypes */

void bufferevent_read_pressure_cb(struct evbuffer *, size_t, size_t, void *);
//就是将ev注册到event_base中去.这个函数内部使用, 主要是bufferevent中的ev_read和ev_write两个event注册到vent_base中去.
static int
bufferevent_add(struct event *ev, int timeout)
{
	struct timeval tv, *ptv = NULL;

	if (timeout) {
		evutil_timerclear(&tv);
		tv.tv_sec = timeout;
		ptv = &tv;
	}

	return (event_add(ev, ptv));
}

/*
 * This callback is executed when the size of the input buffer changes.
 * We use it to apply back pressure on the reading side.
 */

void
bufferevent_read_pressure_cb(struct evbuffer *buf, size_t old, size_t now,
    void *arg) {
	struct bufferevent *bufev = arg;//arg的类型竟然是Bufferevent
	/*
	 * If we are below the watermark then reschedule reading if it's
	 * still enabled.
	 */
	if (bufev->wm_read.high == 0 || now < bufev->wm_read.high) {
		evbuffer_setcb(buf, NULL, NULL);    //这里是将evbuffer类型的buf, buf中的回调函数和回调函数的参数清空.

		if (bufev->enabled & EV_READ)
			bufferevent_add(&bufev->ev_read, bufev->timeout_read);
	}
}
//这个函数是bufferevent类型中ev_read这个event实例的回调函数.这个函数就说:当ev_read就绪之后被处理时,会调用这个函数.
static void
bufferevent_readcb(int fd, short event, void *arg) //arg的类型竟然是bufferevent
{//这个函数的作用,就是当ev_read就绪之后,将从ev_read关联的文件描述符中读取数据,保存到同一个bufferevent结构体中的evbuffer类型的input实例中.然后再讲ev_read的事件注册到框架中去.
	struct bufferevent *bufev = arg;
	int res = 0;
	short what = EVBUFFER_READ;
	size_t len;
	int howmuch = -1;

	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto error;
	}

	/*
	 * If we have a high watermark configured then we don't want to
	 * read more data than would make us reach the watermark.
	 */
	if (bufev->wm_read.high != 0) {//如果bufev设置了wm_read.high字段,就进入.
		howmuch = bufev->wm_read.high - EVBUFFER_LENGTH(bufev->input);
		/* we might have lowered the watermark, stop reading */
		if (howmuch <= 0) {//当bufferevent中的evbuffer类型的input字段的数据的长度比wm_read.high还要大的话,就进入处理.
			struct evbuffer *buf = bufev->input;
			event_del(&bufev->ev_read); //从框架的所有队列中删除这个event实例.
			evbuffer_setcb(buf,
			    bufferevent_read_pressure_cb, bufev);//设置evbuffer类型的input示例的回调函数. arg是与这个buffer关联的bufferevent类型.
			return;
		}
	}
    //下面就是最基本的读
	res = evbuffer_read(bufev->input, fd, howmuch);//从fd中读取howmuch大小的数据,保存到bufferevent的evbuffer类型的input中.
	if (res == -1) {//出错了.
		if (errno == EAGAIN || errno == EINTR)
			goto reschedule;
		/* error case */
		what |= EVBUFFER_ERROR;
	} else if (res == 0) {//读到文件描述符的尾部了.
		/* eof case */
		what |= EVBUFFER_EOF;
	}

	if (res <= 0)
		goto error;
    //将bufferevent类型的event类型的ev_read示例,注册到框架中, 监听读就绪事件.
	bufferevent_add(&bufev->ev_read, bufev->timeout_read);

	/* See if this callbacks meets the water marks */
	len = EVBUFFER_LENGTH(bufev->input);    //evbuffer中的数据长度.
	if (bufev->wm_read.low != 0 && len < bufev->wm_read.low)    //当evbuffer的input中的数据小于wm_read.low就直接返回.
		return;
	if (bufev->wm_read.high != 0 && len >= bufev->wm_read.high) { //如果数据的长度大于high,那么将对应的读event事件在框架的所有的队列中删除这个读event.
		struct evbuffer *buf = bufev->input;
		event_del(&bufev->ev_read);

		/* Now schedule a callback for us when the buffer changes */
		evbuffer_setcb(buf, bufferevent_read_pressure_cb, bufev);//设置这个evbuffer的回调函数.
	}

	/* Invoke the user callback - must always be called last */
	if (bufev->readcb != NULL) //调用bufferevent关联的读的回调函数.
		(*bufev->readcb)(bufev, bufev->cbarg);
	return;

 reschedule:
	bufferevent_add(&bufev->ev_read, bufev->timeout_read);//这里当之前的read操作出错了.那么重新将读事件再次插入到框架中
	return;

 error:
	(*bufev->errorcb)(bufev, what, bufev->cbarg); //出错了,就调用bufferevent的出错的回调函数.
}
//该函数就是当ev_write这event的就绪后的回调函数.函数就是将arg中的output中数据写入到fd中,如果一次没有写完,就在将ev_write注册到框架上.
static void
bufferevent_writecb(int fd, short event, void *arg)
{
	struct bufferevent *bufev = arg;
	int res = 0;
	short what = EVBUFFER_WRITE;

	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto error;
	}

	if (EVBUFFER_LENGTH(bufev->output)) { //如果bufferevent中的output的evbuffer中有数据,就进入了.
	    res = evbuffer_write(bufev->output, fd);//调用写操作.将bufferevent中的output对象保存的数据写入到fd文件描述符中.
	    if (res == -1) {    //出错了.
#ifndef WIN32
/*todo. evbuffer uses WriteFile when WIN32 is set. WIN32 system calls do not
 *set errno. thus this error checking is not portable*/
		    if (errno == EAGAIN ||
			errno == EINTR ||
			errno == EINPROGRESS)
			    goto reschedule;
		    /* error case */
		    what |= EVBUFFER_ERROR;

#else
				goto reschedule;    //重新注册ev_write事件.
#endif

	    } else if (res == 0) {
		    /* eof case */
		    what |= EVBUFFER_EOF;
	    }
	    if (res <= 0)
		    goto error;
	}

	if (EVBUFFER_LENGTH(bufev->output) != 0)    //如果output中还有数据的话,那么就继续注册ev_write事件.
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);

	/*
	 * Invoke the user callback if our buffer is drained or below the
	 * low watermark.
	 */
	if (bufev->writecb != NULL &&
	    EVBUFFER_LENGTH(bufev->output) <= bufev->wm_write.low)
		(*bufev->writecb)(bufev, bufev->cbarg); //调用bufferevent关联的写回调函数.

	return;

 reschedule:
	if (EVBUFFER_LENGTH(bufev->output) != 0)
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);
	return;

 error:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

/*
 * Create a new buffered event object.
 *
 * The read callback is invoked whenever we read new data.
 * The write callback is invoked whenever the output buffer is drained.
 * The error callback is invoked on a write/read error or on EOF.
 *
 * Both read and write callbacks maybe NULL.  The error callback is not
 * allowed to be NULL and have to be provided always.
 */
//创建一个新的bufferevent实例.
struct bufferevent *
bufferevent_new(int fd, evbuffercb readcb, evbuffercb writecb,
    everrorcb errorcb, void *cbarg)
{
	struct bufferevent *bufev;

	if ((bufev = calloc(1, sizeof(struct bufferevent))) == NULL)
		return (NULL);

	if ((bufev->input = evbuffer_new()) == NULL) {
		free(bufev);
		return (NULL);
	}

	if ((bufev->output = evbuffer_new()) == NULL) {
		evbuffer_free(bufev->input);
		free(bufev);
		return (NULL);
	}

	event_set(&bufev->ev_read, fd, EV_READ, bufferevent_readcb, bufev);
	event_set(&bufev->ev_write, fd, EV_WRITE, bufferevent_writecb, bufev);

	bufferevent_setcb(bufev, readcb, writecb, errorcb, cbarg);

	/* 创建bufferevent就设置EV_write,保证写就绪时,就可以自动被调用.读就绪的标志位,需要显性设置.
	 * Set to EV_WRITE so that using bufferevent_write is going to
	 * trigger a callback.  Reading needs to be explicitly enabled
	 * because otherwise no data will be available.
	 */
	bufev->enabled = EV_WRITE;

	return (bufev);
}
//设置bufferevent的三个回调函数和cbarg
void
bufferevent_setcb(struct bufferevent *bufev,
    evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg)
{
	bufev->readcb = readcb;
	bufev->writecb = writecb;
	bufev->errorcb = errorcb;

	bufev->cbarg = cbarg;
}
//设置bufferevent的ev_write和ev_read两个事件关注的文件描述符.现在列表中删除两个event,更新event结构体.将base关联到这两个event结构体中.
void
bufferevent_setfd(struct bufferevent *bufev, int fd)
{
	event_del(&bufev->ev_read);
	event_del(&bufev->ev_write);

	event_set(&bufev->ev_read, fd, EV_READ, bufferevent_readcb, bufev);
	event_set(&bufev->ev_write, fd, EV_WRITE, bufferevent_writecb, bufev);
	if (bufev->ev_base != NULL) {
		event_base_set(bufev->ev_base, &bufev->ev_read);
		event_base_set(bufev->ev_base, &bufev->ev_write);
	}

	/* might have to manually trigger event registration */
}
//设置两个event的优先级.
int
bufferevent_priority_set(struct bufferevent *bufev, int priority)
{
	if (event_priority_set(&bufev->ev_read, priority) == -1)
		return (-1);
	if (event_priority_set(&bufev->ev_write, priority) == -1)
		return (-1);

	return (0);
}

/* Closing the file descriptor is the responsibility of the caller */
//析构bufferevent内存.
void
bufferevent_free(struct bufferevent *bufev)
{
	event_del(&bufev->ev_read);
	event_del(&bufev->ev_write);

	evbuffer_free(bufev->input);
	evbuffer_free(bufev->output);

	free(bufev);
}

/*
 * Returns 0 on success;
 *        -1 on failure.
 */
//外部函数,将data指向的数据追加到bufev中.然后往框架中注册对应的ev_write这个事件.
int
bufferevent_write(struct bufferevent *bufev, const void *data, size_t size)
{
	int res;
    //将data数据追加到bufev上.
	res = evbuffer_add(bufev->output, data, size);

	if (res == -1)
		return (res);
    //然后将ev_write事件注册到框架中.
	/* If everything is okay, we need to schedule a write */
	if (size > 0 && (bufev->enabled & EV_WRITE))
		bufferevent_add(&bufev->ev_write, bufev->timeout_write);

	return (res);
}
//将buf中的数据追加到bufev中,然后调用上面的那个函数,最后清空buf参数.
int
bufferevent_write_buffer(struct bufferevent *bufev, struct evbuffer *buf)
{
	int res;

	res = bufferevent_write(bufev, buf->buffer, buf->off);
	if (res != -1)
		evbuffer_drain(buf, buf->off);

	return (res);
}
//将bufferevent中的input缓存中的数据复制到data内存上,最后清空input字段.
size_t
bufferevent_read(struct bufferevent *bufev, void *data, size_t size)
{
	struct evbuffer *buf = bufev->input;

	if (buf->off < size)
		size = buf->off;

	/* Copy the available data to the user buffer */
	memcpy(data, buf->buffer, size);

	if (size)
		evbuffer_drain(buf, size);

	return (size);
}
//函数调用就是根据参数event的值, 把bufferevent中的两个事件event, 注册到框架中.
int
bufferevent_enable(struct bufferevent *bufev, short event)
{
	if (event & EV_READ) {
		if (bufferevent_add(&bufev->ev_read, bufev->timeout_read) == -1)
			return (-1);
	}
	if (event & EV_WRITE) {
		if (bufferevent_add(&bufev->ev_write, bufev->timeout_write) == -1)
			return (-1);
	}

	bufev->enabled |= event;
	return (0);
}
//根据参数event的值,将对应event从框架中删除.
int
bufferevent_disable(struct bufferevent *bufev, short event)
{
	if (event & EV_READ) {
		if (event_del(&bufev->ev_read) == -1)
			return (-1);
	}
	if (event & EV_WRITE) {
		if (event_del(&bufev->ev_write) == -1)
			return (-1);
	}

	bufev->enabled &= ~event;
	return (0);
}

/*
 * Sets the read and write timeout for a buffered event.
 */
//这个函数还会注册读写事件.
void
bufferevent_settimeout(struct bufferevent *bufev,
    int timeout_read, int timeout_write) {
	bufev->timeout_read = timeout_read;
	bufev->timeout_write = timeout_write;

	if (event_pending(&bufev->ev_read, EV_READ, NULL))  //判断ev_read是否关注EV_READ就绪事件
		bufferevent_add(&bufev->ev_read, timeout_read);
	if (event_pending(&bufev->ev_write, EV_WRITE, NULL))    //判读ev_write是否关注EV_WRITE就绪事件.
		bufferevent_add(&bufev->ev_write, timeout_write);
}

/*
 * Sets the water marks
 */
//设置watermark的值.
void
bufferevent_setwatermark(struct bufferevent *bufev, short events,
    size_t lowmark, size_t highmark)
{
	if (events & EV_READ) {
		bufev->wm_read.low = lowmark;
		bufev->wm_read.high = highmark;
	}

	if (events & EV_WRITE) {
		bufev->wm_write.low = lowmark;
		bufev->wm_write.high = highmark;
	}

	/* If the watermarks changed then see if we should call read again */
	bufferevent_read_pressure_cb(bufev->input,
	    0, EVBUFFER_LENGTH(bufev->input), bufev);
}
//bufferevent与event_base相互关联, 函数作用.
int
bufferevent_base_set(struct event_base *base, struct bufferevent *bufev)
{
	int res;

	bufev->ev_base = base;

	res = event_base_set(base, &bufev->ev_read);
	if (res == -1)
		return (res);

	res = event_base_set(base, &bufev->ev_write);
	return (res);
}
