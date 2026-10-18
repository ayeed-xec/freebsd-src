/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 1999,2000 Jonathan Lemon <jlemon@FreeBSD.org>
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _SYS_EVENTVAR_H_
#define _SYS_EVENTVAR_H_

#ifndef _KERNEL
#error "no user-serviceable parts inside"
#endif

#include <sys/_lock.h>
#include <sys/_sx.h>
#include <sys/_task.h>

#define KQ_NEVENTS	8		/* minimize copy{in,out} calls */
#define KQEXTENT	256		/* linear growth by this amount */

struct kqueue {
	struct		mtx kq_lock;
	int		kq_refcnt;
	TAILQ_ENTRY(kqueue)	kq_list;
	TAILQ_HEAD(, knote)	kq_head;	/* list of pending event */
	int		kq_count;		/* number of pending events */
	struct		selinfo kq_sel;
	struct		sigio *kq_sigio;
	struct		filedesc *kq_fdp;
	int		kq_state;
#define KQ_SEL		0x01
#define KQ_SLEEP	0x02
#define KQ_FLUXWAIT	0x04			/* waiting for a in flux kn */
#define KQ_ASYNC	0x08
#define KQ_CLOSING	0x10
#define	KQ_TASKSCHED	0x20			/* task scheduled */
#define	KQ_TASKDRAIN	0x40			/* waiting for task to drain */
#define	KQ_CPONFORK	0x80
#define	KQ_EPOLL	0x100			/* epoll(2) instance */
	int		kq_knlistsize;		/* size of knlist */
	struct		klist *kq_knlist;	/* list of knotes */
	u_long		kq_knhashmask;		/* size of knhash */
	struct		klist *kq_knhash;	/* hash table for knotes */
	struct		task kq_task;
	struct		ucred *kq_cred;
	struct		kqueue *kq_forksrc;
	struct		sx kq_epoll_sx;		/* KQ_EPOLL: epoll_ctl(2) */
};

struct epoll_event;
struct timespec;

typedef int	kqueue_epoll_copyout_t(void *arg, struct epoll_event *ev,
		    int count);

int	kqueue_epoll_acquire(struct file *fp, struct kqueue **kqp);
int	kqueue_epoll_alloc(struct thread *td, struct file *fp);
int	kqueue_epoll_check_nest(struct kqueue *kq, struct file *fp,
	    struct thread *td);
bool	kqueue_epoll_lookup(struct kqueue *kq, int fd, uint64_t *eventsp);
int	kqueue_epoll_register(struct kqueue *kq, struct kevent *kev,
	    struct thread *td);
void	kqueue_epoll_release(struct kqueue *kq);
int	kqueue_epoll_scan(struct kqueue *kq, int maxevents,
	    kqueue_epoll_copyout_t *copyout, void *arg,
	    const struct timespec *tsp, struct thread *td);

#endif /* !_SYS_EVENTVAR_H_ */
