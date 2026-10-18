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

#include <sys/param.h>
#include <sys/epoll.h>
#include <sys/event.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void
add_fd(int ep, int fd, uint32_t events, uint64_t data)
{
	struct epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = events;
	ev.data.u64 = data;
	ATF_REQUIRE_MSG(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0,
	    "epoll_ctl ADD: %s", strerror(errno));
}

static int
wait_events(int ep, struct epoll_event *evs, int n, int timeout)
{
	int rv;

	rv = epoll_wait(ep, evs, n, timeout);
	ATF_REQUIRE_MSG(rv >= 0, "epoll_wait: %s", strerror(errno));
	return (rv);
}

static void
write_byte(int fd)
{
	ATF_REQUIRE(write(fd, "x", 1) == 1);
}

static int64_t
now_ms(void)
{
	struct timespec ts;

	ATF_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

ATF_TC_WITHOUT_HEAD(create);
ATF_TC_BODY(create, tc)
{
	struct stat sb;
	int ep;

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE((fcntl(ep, F_GETFD) & FD_CLOEXEC) == 0);
	ATF_REQUIRE(fstat(ep, &sb) == 0);
	ATF_REQUIRE(S_ISFIFO(sb.st_mode));
	ATF_REQUIRE(close(ep) == 0);

	ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE((fcntl(ep, F_GETFD) & FD_CLOEXEC) != 0);
	ATF_REQUIRE(close(ep) == 0);

	ep = epoll_create(1);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(close(ep) == 0);

	ATF_REQUIRE_ERRNO(EINVAL, epoll_create1(O_NONBLOCK) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_create(0) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_create(-1) == -1);
}

ATF_TC_WITHOUT_HEAD(ctl_errors);
ATF_TC_BODY(ctl_errors, tc)
{
	struct epoll_event ev;
	int dir, ep, file, kq, p[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;

	ATF_REQUIRE_ERRNO(EBADF, epoll_ctl(-1, EPOLL_CTL_ADD, p[0], &ev) == -1);
	ATF_REQUIRE_ERRNO(EBADF, epoll_ctl(ep, EPOLL_CTL_ADD, -1, &ev) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(p[1], EPOLL_CTL_ADD, p[0], &ev) ==
	    -1);
	kq = kqueue();
	ATF_REQUIRE(kq >= 0);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(kq, EPOLL_CTL_ADD, p[0], &ev) ==
	    -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(ep, 42, p[0], &ev) == -1);
	ATF_REQUIRE_ERRNO(EFAULT, epoll_ctl(ep, EPOLL_CTL_ADD, p[0],
	    (struct epoll_event *)1) == -1);

	ATF_REQUIRE_ERRNO(ENOENT, epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) ==
	    -1);
	ATF_REQUIRE_ERRNO(ENOENT, epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) ==
	    -1);
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
	ATF_REQUIRE_ERRNO(EEXIST, epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) ==
	    -1);
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
	ATF_REQUIRE_ERRNO(ENOENT, epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) ==
	    -1);

	file = open("file", O_RDWR | O_CREAT, 0644);
	ATF_REQUIRE(file >= 0);
	ATF_REQUIRE_ERRNO(EPERM, epoll_ctl(ep, EPOLL_CTL_ADD, file, &ev) ==
	    -1);
	dir = open(".", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(dir >= 0);
	ATF_REQUIRE_ERRNO(EPERM, epoll_ctl(ep, EPOLL_CTL_ADD, dir, &ev) ==
	    -1);
}

ATF_TC_WITHOUT_HEAD(exclusive);
ATF_TC_BODY(exclusive, tc)
{
	struct epoll_event ev;
	int ep, ep2, p[2];

	ep = epoll_create1(0);
	ep2 = epoll_create1(0);
	ATF_REQUIRE(ep >= 0 && ep2 >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	memset(&ev, 0, sizeof(ev));

	ev.events = EPOLLEXCLUSIVE | EPOLLIN | EPOLLPRI;
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) ==
	    -1);
	ev.events = EPOLLEXCLUSIVE | EPOLLIN;
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(ep, EPOLL_CTL_ADD, ep2, &ev) ==
	    -1);
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) ==
	    -1);
	ev.events = EPOLLIN;
	ATF_REQUIRE_ERRNO(EINVAL, epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) ==
	    -1);
	write_byte(p[1]);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
}

ATF_TC_WITHOUT_HEAD(wait_errors);
ATF_TC_BODY(wait_errors, tc)
{
	struct epoll_event ev;
	struct timespec ts;
	int ep, kq, p[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	kq = kqueue();
	ATF_REQUIRE(kq >= 0);

	ATF_REQUIRE_ERRNO(EBADF, epoll_wait(-1, &ev, 1, 0) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_wait(p[0], &ev, 1, 0) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_wait(kq, &ev, 1, 0) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_wait(ep, &ev, 0, 0) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_wait(ep, &ev, -1, 0) == -1);
	ATF_REQUIRE_ERRNO(EINVAL, epoll_wait(ep, &ev, INT_MAX, 0) == -1);
	ts.tv_sec = 0;
	ts.tv_nsec = 2000000000L;
	ATF_REQUIRE_ERRNO(EINVAL, epoll_pwait2(ep, &ev, 1, &ts, NULL) == -1);
	ATF_REQUIRE_ERRNO(EFAULT, epoll_pwait2(ep, &ev, 1,
	    (struct timespec *)1, NULL) == -1);
	ATF_REQUIRE_ERRNO(EFAULT, epoll_pwait(ep, &ev, 1, 0,
	    (sigset_t *)1) == -1);

	add_fd(ep, p[0], EPOLLIN, 1);
	write_byte(p[1]);
	ATF_REQUIRE_ERRNO(EFAULT, epoll_wait(ep, (struct epoll_event *)1, 1,
	    0) == -1);
}

ATF_TC_WITHOUT_HEAD(level_triggered);
ATF_TC_BODY(level_triggered, tc)
{
	struct epoll_event ev;
	int ep, p[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(ep, p[0], EPOLLIN, 0x123456789abcdef0ULL);

	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	write_byte(p[1]);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	ATF_REQUIRE_EQ(0x123456789abcdef0ULL, ev.data.u64);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
}

ATF_TC_WITHOUT_HEAD(one_event_per_fd);
ATF_TC_BODY(one_event_per_fd, tc)
{
	struct epoll_event evs[4];
	int ep, s[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, s) == 0);
	add_fd(ep, s[0], EPOLLIN | EPOLLOUT, 7);

	ATF_REQUIRE_EQ(1, wait_events(ep, evs, 4, 0));
	ATF_REQUIRE_EQ(EPOLLOUT, evs[0].events);
	write_byte(s[1]);
	ATF_REQUIRE_EQ(1, wait_events(ep, evs, 4, 0));
	ATF_REQUIRE_EQ(EPOLLIN | EPOLLOUT, evs[0].events);
	ATF_REQUIRE_EQ(7, evs[0].data.u64);
}

ATF_TC_WITHOUT_HEAD(pipe_hangup);
ATF_TC_BODY(pipe_hangup, tc)
{
	struct epoll_event ev;
	int ep, p[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(ep, p[0], EPOLLIN, 1);
	ATF_REQUIRE(close(p[1]) == 0);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 1000));
	ATF_REQUIRE((ev.events & EPOLLHUP) != 0);
	ATF_REQUIRE((ev.events & ~(EPOLLIN | EPOLLHUP)) == 0);
}

ATF_TC_WITHOUT_HEAD(no_events_hangup);
ATF_TC_BODY(no_events_hangup, tc)
{
	struct epoll_event ev;
	int ep, s[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, s) == 0);
	add_fd(ep, s[0], 0, 3);
	write_byte(s[1]);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE(close(s[1]) == 0);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 1000));
	ATF_REQUIRE_EQ(EPOLLHUP, ev.events);
	ATF_REQUIRE_EQ(3, ev.data.u64);
}

ATF_TC_WITHOUT_HEAD(rdhup);
ATF_TC_BODY(rdhup, tc)
{
	struct epoll_event ev;
	int ep, s[2], t[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, s) == 0);
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, t) == 0);
	add_fd(ep, s[0], EPOLLIN | EPOLLRDHUP, 1);
	add_fd(ep, t[0], EPOLLIN, 2);
	ATF_REQUIRE(shutdown(s[1], SHUT_WR) == 0);
	ATF_REQUIRE(shutdown(t[1], SHUT_WR) == 0);

	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 1000));
	if (ev.data.u64 == 2)
		ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	else
		ATF_REQUIRE_EQ(EPOLLIN | EPOLLRDHUP, ev.events);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 1000));
	if (ev.data.u64 == 2)
		ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	else
		ATF_REQUIRE_EQ(EPOLLIN | EPOLLRDHUP, ev.events);
}

ATF_TC_WITHOUT_HEAD(edge_triggered);
ATF_TC_BODY(edge_triggered, tc)
{
	struct epoll_event ev;
	int ep, p[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(ep, p[0], EPOLLIN | EPOLLET, 1);

	write_byte(p[1]);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	write_byte(p[1]);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
}

ATF_TC_WITHOUT_HEAD(oneshot);
ATF_TC_BODY(oneshot, tc)
{
	struct epoll_event ev;
	int ep, s[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, s) == 0);
	add_fd(ep, s[0], EPOLLIN | EPOLLOUT | EPOLLONESHOT, 1);

	write_byte(s[1]);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN | EPOLLOUT, ev.events);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	write_byte(s[1]);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));

	ev.events = EPOLLIN | EPOLLONESHOT;
	ev.data.u64 = 2;
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_MOD, s[0], &ev) == 0);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	ATF_REQUIRE_EQ(2, ev.data.u64);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
}

ATF_TC_WITHOUT_HEAD(mod_and_del);
ATF_TC_BODY(mod_and_del, tc)
{
	struct epoll_event ev;
	int ep, p[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(ep, p[0], EPOLLIN, 1);
	write_byte(p[1]);

	ev.events = EPOLLIN;
	ev.data.u64 = 2;
	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(2, ev.data.u64);

	ATF_REQUIRE(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
}

struct waiter {
	int	ep;
	int	rv;
	int64_t	ms;
};

static void *
waiter_main(void *arg)
{
	struct waiter *w;
	struct epoll_event ev;
	int64_t start;

	w = arg;
	start = now_ms();
	w->rv = epoll_wait(w->ep, &ev, 1, 5000);
	w->ms = now_ms() - start;
	return (NULL);
}

ATF_TC_WITHOUT_HEAD(add_wakes_waiter);
ATF_TC_BODY(add_wakes_waiter, tc)
{
	struct waiter w;
	pthread_t thr;
	int p[2];

	w.ep = epoll_create1(0);
	ATF_REQUIRE(w.ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	write_byte(p[1]);
	ATF_REQUIRE(pthread_create(&thr, NULL, waiter_main, &w) == 0);
	usleep(100000);
	add_fd(w.ep, p[0], EPOLLIN, 1);
	ATF_REQUIRE(pthread_join(thr, NULL) == 0);
	ATF_REQUIRE_EQ(1, w.rv);
	ATF_REQUIRE(w.ms < 2000);
}

ATF_TC_WITHOUT_HEAD(maxevents_one);
ATF_TC_BODY(maxevents_one, tc)
{
	struct epoll_event ev;
	uint64_t seen;
	int ep, p[2], q[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	ATF_REQUIRE(pipe(q) == 0);
	add_fd(ep, p[0], EPOLLIN, 1);
	add_fd(ep, q[0], EPOLLIN, 2);
	write_byte(p[1]);
	write_byte(q[1]);

	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	seen = ev.data.u64;
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	seen |= ev.data.u64;
	ATF_REQUIRE_EQ(3, seen);
}

ATF_TC_WITHOUT_HEAD(timeouts);
ATF_TC_BODY(timeouts, tc)
{
	struct epoll_event ev;
	struct timespec ts;
	int64_t start;
	int ep;

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	start = now_ms();
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE(now_ms() - start < 50);

	start = now_ms();
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 100));
	ATF_REQUIRE(now_ms() - start >= 100);

	ts.tv_sec = 0;
	ts.tv_nsec = 500;
	ATF_REQUIRE_EQ(0, epoll_pwait2(ep, &ev, 1, &ts, NULL));
}

static volatile sig_atomic_t got_signal;

static void
on_signal(int sig __unused)
{
	got_signal = 1;
}

ATF_TC_WITHOUT_HEAD(signals);
ATF_TC_BODY(signals, tc)
{
	struct epoll_event ev;
	struct sigaction sa;
	struct itimerval it;
	sigset_t mask;
	int ep;

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sa.sa_flags = SA_RESTART;
	ATF_REQUIRE(sigaction(SIGALRM, &sa, NULL) == 0);
	ATF_REQUIRE(sigaction(SIGUSR1, &sa, NULL) == 0);

	/* A signal ends the wait with EINTR, also with SA_RESTART. */
	memset(&it, 0, sizeof(it));
	it.it_value.tv_usec = 100000;
	ATF_REQUIRE(setitimer(ITIMER_REAL, &it, NULL) == 0);
	ATF_REQUIRE_ERRNO(EINTR, epoll_wait(ep, &ev, 1, -1) == -1);
	ATF_REQUIRE(got_signal);

	/* The mask of epoll_pwait() holds the signal during the wait. */
	got_signal = 0;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	ATF_REQUIRE(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
	ATF_REQUIRE(kill(getpid(), SIGUSR1) == 0);
	ATF_REQUIRE(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
	ATF_REQUIRE_EQ(0, epoll_pwait(ep, &ev, 1, 50, &mask));
	ATF_REQUIRE(!got_signal);
	sigemptyset(&mask);
	ATF_REQUIRE_ERRNO(EINTR, epoll_pwait(ep, &ev, 1, 1000, &mask) == -1);
	ATF_REQUIRE(got_signal);
}

ATF_TC_WITHOUT_HEAD(nested);
ATF_TC_BODY(nested, tc)
{
	struct epoll_event ev;
	int eps[7], i, p[2];

	for (i = 0; i < 7; i++) {
		eps[i] = epoll_create1(0);
		ATF_REQUIRE(eps[i] >= 0);
	}
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(eps[0], p[0], EPOLLIN, 1);
	add_fd(eps[1], eps[0], EPOLLIN, 10);
	write_byte(p[1]);
	ATF_REQUIRE_EQ(1, wait_events(eps[1], &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	ATF_REQUIRE_EQ(10, ev.data.u64);

	/* A loop of two instances. */
	ev.events = EPOLLIN;
	ev.data.u64 = 0;
	ATF_REQUIRE_ERRNO(ELOOP, epoll_ctl(eps[0], EPOLL_CTL_ADD, eps[1],
	    &ev) == -1);

	/* A chain of 5 instances is allowed, a chain of 6 is not. */
	for (i = 2; i < 5; i++)
		add_fd(eps[i], eps[i - 1], EPOLLIN, i);
	ATF_REQUIRE_ERRNO(ELOOP, epoll_ctl(eps[5], EPOLL_CTL_ADD, eps[4],
	    &ev) == -1);
}

ATF_TC_WITHOUT_HEAD(close_ends_registration);
ATF_TC_BODY(close_ends_registration, tc)
{
	struct epoll_event ev;
	int ep, fd, p[2], q[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(ep, p[0], EPOLLIN, 1);
	write_byte(p[1]);
	fd = p[0];
	ATF_REQUIRE(close(p[0]) == 0);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_ERRNO(EBADF, epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL) == -1);

	/* The same number again gets a new registration. */
	ATF_REQUIRE(pipe(q) == 0);
	ATF_REQUIRE_EQ(fd, q[0]);
	add_fd(ep, q[0], EPOLLIN, 2);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
}

ATF_TC_WITHOUT_HEAD(fork_copy);
ATF_TC_BODY(fork_copy, tc)
{
	struct epoll_event ev;
	pid_t pid;
	int ep, p[2], status;

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ATF_REQUIRE(pipe(p) == 0);
	add_fd(ep, p[0], EPOLLIN, 5);
	write_byte(p[1]);

	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		if (epoll_wait(ep, &ev, 1, 1000) != 1 || ev.data.u64 != 5)
			_exit(1);
		_exit(0);
	}
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	ATF_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
}

ATF_TC_WITHOUT_HEAD(eventfd_wake);
ATF_TC_BODY(eventfd_wake, tc)
{
	struct epoll_event ev;
	uint64_t one;
	int efd, ep;

	ep = epoll_create1(EPOLL_CLOEXEC);
	ATF_REQUIRE(ep >= 0);
	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	ATF_REQUIRE(efd >= 0);
	add_fd(ep, efd, EPOLLIN, 1);
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
	one = 1;
	ATF_REQUIRE(write(efd, &one, sizeof(one)) == sizeof(one));
	ATF_REQUIRE_EQ(1, wait_events(ep, &ev, 1, 0));
	ATF_REQUIRE_EQ(EPOLLIN, ev.events);
	ATF_REQUIRE(read(efd, &one, sizeof(one)) == sizeof(one));
	ATF_REQUIRE_EQ(0, wait_events(ep, &ev, 1, 0));
}

/*
 * kevent(2) cannot see an epoll instance, and a kqueue keeps its own
 * behavior: one kevent for each filter.
 */
ATF_TC_WITHOUT_HEAD(kqueue_unchanged);
ATF_TC_BODY(kqueue_unchanged, tc)
{
	struct kevent kev[2], out[4];
	struct timespec ts;
	int ep, kq, s[2];

	ep = epoll_create1(0);
	ATF_REQUIRE(ep >= 0);
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	ATF_REQUIRE_ERRNO(EINVAL, kevent(ep, NULL, 0, out, 4, &ts) == -1);

	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_STREAM, 0, s) == 0);
	write_byte(s[1]);
	kq = kqueue();
	ATF_REQUIRE(kq >= 0);
	EV_SET(&kev[0], s[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
	EV_SET(&kev[1], s[0], EVFILT_WRITE, EV_ADD, 0, 0, NULL);
	ATF_REQUIRE(kevent(kq, kev, 2, NULL, 0, NULL) == 0);
	ATF_REQUIRE_EQ(2, kevent(kq, NULL, 0, out, 4, &ts));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, create);
	ATF_TP_ADD_TC(tp, ctl_errors);
	ATF_TP_ADD_TC(tp, exclusive);
	ATF_TP_ADD_TC(tp, wait_errors);
	ATF_TP_ADD_TC(tp, level_triggered);
	ATF_TP_ADD_TC(tp, one_event_per_fd);
	ATF_TP_ADD_TC(tp, pipe_hangup);
	ATF_TP_ADD_TC(tp, no_events_hangup);
	ATF_TP_ADD_TC(tp, rdhup);
	ATF_TP_ADD_TC(tp, edge_triggered);
	ATF_TP_ADD_TC(tp, oneshot);
	ATF_TP_ADD_TC(tp, mod_and_del);
	ATF_TP_ADD_TC(tp, add_wakes_waiter);
	ATF_TP_ADD_TC(tp, maxevents_one);
	ATF_TP_ADD_TC(tp, timeouts);
	ATF_TP_ADD_TC(tp, signals);
	ATF_TP_ADD_TC(tp, nested);
	ATF_TP_ADD_TC(tp, close_ends_registration);
	ATF_TP_ADD_TC(tp, fork_copy);
	ATF_TP_ADD_TC(tp, eventfd_wake);
	ATF_TP_ADD_TC(tp, kqueue_unchanged);
	return (atf_no_error());
}
