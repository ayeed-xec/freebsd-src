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

#ifndef _SYS_EPOLL_H_
#define	_SYS_EPOLL_H_

#include <sys/cdefs.h>
#include <sys/_types.h>
#include <sys/_sigset.h>
#include <sys/_timespec.h>

#define	EPOLL_CLOEXEC	0x00100000	/* same value as O_CLOEXEC */

#define	EPOLL_CTL_ADD	1
#define	EPOLL_CTL_DEL	2
#define	EPOLL_CTL_MOD	3

#define	EPOLLIN		0x00000001
#define	EPOLLPRI	0x00000002
#define	EPOLLOUT	0x00000004
#define	EPOLLERR	0x00000008
#define	EPOLLHUP	0x00000010
#define	EPOLLRDNORM	0x00000040
#define	EPOLLRDBAND	0x00000080
#define	EPOLLWRNORM	0x00000100
#define	EPOLLWRBAND	0x00000200
#define	EPOLLMSG	0x00000400
#define	EPOLLRDHUP	0x00002000
#define	EPOLLEXCLUSIVE	0x10000000
#define	EPOLLWAKEUP	0x20000000
#define	EPOLLONESHOT	0x40000000
#define	EPOLLET		0x80000000

typedef union epoll_data {
	void		*ptr;
	int		fd;
	__uint32_t	u32;
	__uint64_t	u64;
} epoll_data_t;

/* Packed on amd64 only, so that the layout is the one of Linux. */
struct epoll_event {
	__uint32_t	events;
	epoll_data_t	data;
}
#ifdef __amd64__
__packed
#endif
;

#ifdef _KERNEL

struct file;
struct thread;

/* Events that epoll_ctl(2) accepts. */
#define	EPOLL_EVENTS_IN		(EPOLLIN | EPOLLPRI | EPOLLRDNORM | \
	    EPOLLRDBAND | EPOLLRDHUP)
#define	EPOLL_EVENTS_OUT	(EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)
#define	EPOLL_EVENTS_ALL	(EPOLL_EVENTS_IN | EPOLL_EVENTS_OUT | \
	    EPOLLERR | EPOLLHUP | EPOLLMSG | EPOLLEXCLUSIVE | EPOLLWAKEUP | \
	    EPOLLONESHOT | EPOLLET)

/* The longest chain of nested instances. */
#define	EPOLL_MAX_NESTS		5

int	epoll_create_file(struct thread *td, struct file *fp, int flags);
__uint32_t epoll_poll_events(struct file *fp, __uint32_t events,
	    struct thread *td);
int	kern_epoll_ctl(struct thread *td, int epfd, int op, int fd,
	    struct epoll_event *event);
int	kern_epoll_wait(struct thread *td, int epfd,
	    struct epoll_event *events, int maxevents,
	    const struct timespec *timeout, __sigset_t *sigmask);

#else /* !_KERNEL */

#ifndef _SIGSET_T_DECLARED
#define	_SIGSET_T_DECLARED
typedef	__sigset_t	sigset_t;
#endif

__BEGIN_DECLS
int	epoll_create(int);
int	epoll_create1(int);
int	epoll_ctl(int, int, int, struct epoll_event *);
int	epoll_pwait(int, struct epoll_event *, int, int, const sigset_t *);
int	epoll_pwait2(int, struct epoll_event *, int, const struct timespec *,
	    const sigset_t *);
int	epoll_wait(int, struct epoll_event *, int, int);
__END_DECLS

#endif /* !_KERNEL */

#endif /* !_SYS_EPOLL_H_ */
