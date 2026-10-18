/*
 * Copyright (c) 2026 Ayeed Shaikh <Ayeed.AI10@race.reva.edu.in>
 *
 * SPDX-License-Identifier: BSD-2-Clause
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

/*
 * epoll(2): the Linux event interface on top of kqueue.  kern_event.c keeps
 * the instances and the scan; this file has the system calls, the checks of
 * their arguments and the conversion between epoll events and poll events.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/capsicum.h>
#include <sys/epoll.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/selinfo.h>
#include <sys/eventvar.h>	/* Must come after sys/selinfo.h */
#include <sys/signalvar.h>
#include <sys/sx.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>
#include <sys/vnode.h>

#define	EPOLL_MAXEVENTS	((int)(INT_MAX / sizeof(struct epoll_event)))

/* Events that may come with EPOLLEXCLUSIVE. */
#define	EPOLL_EXCLUSIVE_OK	(EPOLLEXCLUSIVE | EPOLLIN | EPOLLOUT | \
	    EPOLLWAKEUP | EPOLLET | EPOLLERR | EPOLLHUP)

struct epoll_copyout_args {
	struct epoll_event	*uevents;
	int			count;
};

/*
 * Registrations of a kqueue in an epoll instance are serialized, so that
 * the check for loops and the registration are one step.
 */
static struct sx epoll_nest_sx;
SX_SYSINIT(epoll_nest, &epoll_nest_sx, "epollnest");

static int	epoll_add(struct kqueue *kq, struct file *fp, int fd,
		    const struct epoll_event *event, struct thread *td);
static int	epoll_copyout(void *arg, struct epoll_event *ev, int count);
static void	epoll_delete(struct kqueue *kq, int fd, struct thread *td);

int
epoll_create_file(struct thread *td, struct file *fp, int flags)
{

	if ((flags & ~EPOLL_CLOEXEC) != 0)
		return (EINVAL);
	return (kqueue_epoll_alloc(td, fp));
}

/*
 * Return the events of fp that a registration for "events" reports now.
 */
uint32_t
epoll_poll_events(struct file *fp, uint32_t events, struct thread *td)
{
	uint32_t ev;
	int pevents, revents;

	pevents = POLLERR | POLLHUP;
	if ((events & EPOLLIN) != 0)
		pevents |= POLLIN;
	if ((events & EPOLLPRI) != 0)
		pevents |= POLLPRI;
	if ((events & (EPOLLOUT | EPOLLWRNORM)) != 0)
		pevents |= POLLOUT;
	if ((events & EPOLLRDNORM) != 0)
		pevents |= POLLRDNORM;
	if ((events & EPOLLRDBAND) != 0)
		pevents |= POLLRDBAND;
	if ((events & EPOLLWRBAND) != 0)
		pevents |= POLLWRBAND;
	if ((events & EPOLLRDHUP) != 0)
		pevents |= POLLRDHUP;

	revents = poll_file_norecord(fp, pevents, td->td_ucred, td);

	ev = 0;
	if ((revents & POLLIN) != 0)
		ev |= EPOLLIN;
	if ((revents & POLLPRI) != 0)
		ev |= EPOLLPRI;
	if ((revents & POLLOUT) != 0)
		ev |= EPOLLOUT | EPOLLWRNORM;
	if ((revents & POLLRDNORM) != 0)
		ev |= EPOLLRDNORM;
	if ((revents & POLLRDBAND) != 0)
		ev |= EPOLLRDBAND;
	if ((revents & POLLWRBAND) != 0)
		ev |= EPOLLWRBAND;
	if ((revents & POLLRDHUP) != 0)
		ev |= EPOLLRDHUP;
	if ((revents & (POLLERR | POLLNVAL)) != 0)
		ev |= EPOLLERR;
	if ((revents & POLLHUP) != 0)
		ev |= EPOLLHUP;
	return (ev & (events | EPOLLERR | EPOLLHUP));
}

/*
 * Add the knotes of a registration.  Reading and writing use one knote each;
 * a registration with no input and no output events uses the knote of the
 * direction that the descriptor allows, so that it still reports EPOLLERR
 * and EPOLLHUP.
 */
static int
epoll_add(struct kqueue *kq, struct file *fp, int fd,
    const struct epoll_event *event, struct thread *td)
{
	struct kevent kev[2];
	uint32_t events;
	int error, flags, i, nkev;
	bool in, out;

	events = event->events;
	if (fp->f_type == DTYPE_VNODE &&
	    (fp->f_vnode->v_type == VREG || fp->f_vnode->v_type == VDIR))
		return (EPERM);

	in = (events & EPOLL_EVENTS_IN) != 0;
	out = (events & EPOLL_EVENTS_OUT) != 0;
	if (!in && !out) {
		if ((fp->f_flag & FREAD) != 0)
			in = true;
		else
			out = true;
	}
	flags = EV_ADD;
	if ((events & EPOLLET) != 0)
		flags |= EV_CLEAR;
	if ((events & EPOLLONESHOT) != 0)
		flags |= EV_DISPATCH;

	nkev = 0;
	if (in) {
		EV_SET(&kev[nkev], fd, EVFILT_READ, flags, 0, 0, NULL);
		nkev++;
	}
	if (out) {
		EV_SET(&kev[nkev], fd, EVFILT_WRITE, flags, 0, 0, NULL);
		nkev++;
	}
	for (i = 0; i < nkev; i++) {
		kev[i].ext[2] = events;
		kev[i].ext[3] = event->data.u64;
	}

	error = kqueue_epoll_check_nest(kq, fp, td);
	if (error != 0)
		return (error);
	for (i = 0; i < nkev; i++) {
		error = kqueue_epoll_register(kq, &kev[i], td);
		if (error != 0)
			break;
	}
	if (error != 0) {
		if (i > 0)
			epoll_delete(kq, fd, td);
		/* The file has no filter for this direction. */
		if (error == EINVAL || error == EOPNOTSUPP)
			error = EPERM;
	}
	return (error);
}

static void
epoll_delete(struct kqueue *kq, int fd, struct thread *td)
{
	struct kevent kev;

	EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	(void)kqueue_epoll_register(kq, &kev, td);
	EV_SET(&kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	(void)kqueue_epoll_register(kq, &kev, td);
}

int
kern_epoll_ctl(struct thread *td, int epfd, int op, int fd,
    struct epoll_event *event)
{
	cap_rights_t rights;
	struct file *epfp, *fp;
	struct kqueue *kq;
	uint64_t oldevents;
	int error;
	bool nest;

	if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL)
		return (EINVAL);

	error = fget(td, epfd, cap_rights_init_one(&rights, CAP_KQUEUE_CHANGE),
	    &epfp);
	if (error != 0)
		return (error);
	error = fget(td, fd, cap_rights_init_one(&rights, CAP_EVENT), &fp);
	if (error != 0)
		goto out_epfp;
	error = kqueue_epoll_acquire(epfp, &kq);
	if (error != 0)
		goto out_fp;
	if (epfp == fp) {
		error = EINVAL;
		goto out_kq;
	}
	if (op != EPOLL_CTL_DEL && (event->events & EPOLLEXCLUSIVE) != 0 &&
	    (op == EPOLL_CTL_MOD ||
	    (event->events & ~EPOLL_EXCLUSIVE_OK) != 0 ||
	    fp->f_type == DTYPE_KQUEUE)) {
		error = EINVAL;
		goto out_kq;
	}

	nest = op != EPOLL_CTL_DEL && fp->f_type == DTYPE_KQUEUE;
	if (nest)
		sx_xlock(&epoll_nest_sx);
	sx_xlock(&kq->kq_epoll_sx);
	switch (op) {
	case EPOLL_CTL_ADD:
		if (kqueue_epoll_lookup(kq, fd, &oldevents))
			error = EEXIST;
		else
			error = epoll_add(kq, fp, fd, event, td);
		break;
	case EPOLL_CTL_MOD:
		if (!kqueue_epoll_lookup(kq, fd, &oldevents)) {
			error = ENOENT;
		} else if ((oldevents & EPOLLEXCLUSIVE) != 0) {
			error = EINVAL;
		} else {
			epoll_delete(kq, fd, td);
			error = epoll_add(kq, fp, fd, event, td);
		}
		break;
	case EPOLL_CTL_DEL:
		if (!kqueue_epoll_lookup(kq, fd, &oldevents))
			error = ENOENT;
		else
			epoll_delete(kq, fd, td);
		break;
	}
	sx_xunlock(&kq->kq_epoll_sx);
	if (nest)
		sx_xunlock(&epoll_nest_sx);

out_kq:
	kqueue_epoll_release(kq);
out_fp:
	fdrop(fp, td);
out_epfp:
	fdrop(epfp, td);
	return (error);
}

static int
epoll_copyout(void *arg, struct epoll_event *ev, int count)
{
	struct epoll_copyout_args *args;
	int error;

	args = arg;
	error = copyout(ev, args->uevents + args->count,
	    count * sizeof(*ev));
	if (error == 0)
		args->count += count;
	return (error);
}

int
kern_epoll_wait(struct thread *td, int epfd, struct epoll_event *events,
    int maxevents, const struct timespec *timeout, sigset_t *sigmask)
{
	struct epoll_copyout_args args;
	cap_rights_t rights;
	struct file *epfp;
	struct kqueue *kq;
	int error;

	if (maxevents <= 0 || maxevents > EPOLL_MAXEVENTS)
		return (EINVAL);

	error = fget(td, epfd, cap_rights_init_one(&rights, CAP_KQUEUE_EVENT),
	    &epfp);
	if (error != 0)
		return (error);
	error = kqueue_epoll_acquire(epfp, &kq);
	if (error != 0)
		goto out;

	if (sigmask != NULL) {
		error = kern_sigprocmask(td, SIG_SETMASK, sigmask,
		    &td->td_oldsigmask, 0);
		if (error != 0)
			goto out_kq;
		td->td_pflags |= TDP_OLDMASK;
	}

	args.uevents = events;
	args.count = 0;
	error = kqueue_epoll_scan(kq, maxevents, epoll_copyout, &args,
	    timeout, td);

	if (sigmask != NULL) {
		/*
		 * Make sure that ast() restores the old signal mask on the
		 * return to user mode, as ppoll(2) does.
		 */
		if (error == EINTR)
			ast_sched(td, TDA_SIGSUSPEND);
		else
			ast_sched(td, TDA_PSELECT);
	}
out_kq:
	kqueue_epoll_release(kq);
out:
	fdrop(epfp, td);
	return (error);
}

int
sys_epoll_ctl(struct thread *td, struct epoll_ctl_args *uap)
{
	struct epoll_event event;
	int error;

	if (uap->op != EPOLL_CTL_DEL) {
		error = copyin(uap->event, &event, sizeof(event));
		if (error != 0)
			return (error);
	}
	return (kern_epoll_ctl(td, uap->epfd, uap->op, uap->fd, &event));
}

int
sys_epoll_pwait2(struct thread *td, struct epoll_pwait2_args *uap)
{
	struct timespec ts, *tsp;
	sigset_t set, *setp;
	int error;

	if (uap->timeout != NULL) {
		error = copyin(uap->timeout, &ts, sizeof(ts));
		if (error != 0)
			return (error);
		tsp = &ts;
	} else
		tsp = NULL;
	if (uap->sigmask != NULL) {
		error = copyin(uap->sigmask, &set, sizeof(set));
		if (error != 0)
			return (error);
		setp = &set;
	} else
		setp = NULL;
	return (kern_epoll_wait(td, uap->epfd, uap->events, uap->maxevents,
	    tsp, setp));
}
