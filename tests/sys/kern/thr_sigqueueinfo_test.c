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
#include <sys/capsicum.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/wait.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static siginfo_t received;
static volatile sig_atomic_t received_count;

static void
handler(int sig __unused, siginfo_t *si, void *uc __unused)
{
	received = *si;
	received_count++;
}

static void
catch_signal(int sig)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	ATF_REQUIRE_EQ(0, sigaction(sig, &sa, NULL));
	received_count = 0;
}

static void
block_signal(int sig)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, sig);
	ATF_REQUIRE_EQ(0, sigprocmask(SIG_BLOCK, &set, NULL));
}

static long
self_id(void)
{
	long id;

	ATF_REQUIRE_EQ(0, thr_self(&id));
	return (id);
}

ATF_TC_WITHOUT_HEAD(self_any_code);
ATF_TC_BODY(self_any_code, tc)
{
	static const int codes[] = { SI_LWP, SI_USER, SI_QUEUE, SEGV_MAPERR };
	siginfo_t si;
	size_t i;

	catch_signal(SIGUSR1);
	for (i = 0; i < nitems(codes); i++) {
		memset(&si, 0, sizeof(si));
		si.si_code = codes[i];
		si.si_pid = 4242;
		si.si_uid = 77;
		si.si_addr = (void *)(uintptr_t)0x1230;
		si.si_value.sival_int = (int)i + 1;
		received_count = 0;
		ATF_REQUIRE_EQ_MSG(0, thr_sigqueueinfo(getpid(), self_id(),
		    SIGUSR1, &si), "thr_sigqueueinfo: %s", strerror(errno));
		ATF_REQUIRE_EQ(1, received_count);
		ATF_CHECK_EQ(SIGUSR1, received.si_signo);
		ATF_CHECK_EQ(codes[i], received.si_code);
		ATF_CHECK_EQ(4242, received.si_pid);
		ATF_CHECK_EQ(77, received.si_uid);
		ATF_CHECK_EQ((void *)(uintptr_t)0x1230, received.si_addr);
		ATF_CHECK_EQ((int)i + 1, received.si_value.sival_int);
	}
}

static long target_id;
static siginfo_t target_si;
static pthread_barrier_t barrier;

static void *
target_main(void *arg __unused)
{
	sigset_t set;

	ATF_REQUIRE_EQ(0, thr_self(&target_id));
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	pthread_barrier_wait(&barrier);
	ATF_REQUIRE_EQ(SIGUSR2, sigwaitinfo(&set, &target_si));
	return (NULL);
}

ATF_TC_WITHOUT_HEAD(other_thread);
ATF_TC_BODY(other_thread, tc)
{
	sigset_t pending;
	pthread_t thr;
	siginfo_t si;

	block_signal(SIGUSR2);
	ATF_REQUIRE_EQ(0, pthread_barrier_init(&barrier, NULL, 2));
	ATF_REQUIRE_EQ(0, pthread_create(&thr, NULL, target_main, NULL));
	pthread_barrier_wait(&barrier);

	memset(&si, 0, sizeof(si));
	si.si_code = SI_QUEUE;
	si.si_value.sival_int = 31;
	ATF_REQUIRE_EQ(0, thr_sigqueueinfo(getpid(), target_id, SIGUSR2, &si));
	ATF_REQUIRE_EQ(0, pthread_join(thr, NULL));
	ATF_CHECK_EQ(SIGUSR2, target_si.si_signo);
	ATF_CHECK_EQ(31, target_si.si_value.sival_int);

	/* The signal went to the target thread only. */
	ATF_REQUIRE_EQ(0, sigpending(&pending));
	ATF_CHECK(!sigismember(&pending, SIGUSR2));
}

/*
 * Fork a child that blocks SIGUSR1, sends its thread id to the parent and
 * waits for the signal.  The child exits with 0 if the siginfo shows the
 * parent as the sender with value 99, and with 2 otherwise.
 */
static pid_t
start_child(long *idp, int *donefd)
{
	siginfo_t si;
	sigset_t set;
	long id;
	pid_t pid;
	int tochild[2], toparent[2];
	char c;

	ATF_REQUIRE_EQ(0, pipe(tochild));
	ATF_REQUIRE_EQ(0, pipe(toparent));
	block_signal(SIGUSR1);
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		sigemptyset(&set);
		sigaddset(&set, SIGUSR1);
		if (thr_self(&id) != 0 ||
		    write(toparent[1], &id, sizeof(id)) != sizeof(id))
			_exit(1);
		if (read(tochild[0], &c, 1) != 1)
			_exit(1);
		if (c == 'q')
			_exit(0);
		if (sigwaitinfo(&set, &si) != SIGUSR1)
			_exit(1);
		if (si.si_code != SI_QUEUE || si.si_pid != getppid() ||
		    si.si_uid != getuid() || si.si_value.sival_int != 99)
			_exit(2);
		_exit(0);
	}
	ATF_REQUIRE_EQ(sizeof(*idp), read(toparent[0], idp, sizeof(*idp)));
	*donefd = tochild[1];
	return (pid);
}

static void
finish_child(pid_t pid, int donefd, char c)
{
	int status;

	ATF_REQUIRE_EQ(1, write(donefd, &c, 1));
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child status %#x", status);
}

ATF_TC_WITHOUT_HEAD(child_queue);
ATF_TC_BODY(child_queue, tc)
{
	siginfo_t si;
	long id;
	pid_t pid;
	int donefd;

	pid = start_child(&id, &donefd);
	memset(&si, 0, sizeof(si));
	si.si_code = SI_QUEUE;
	si.si_pid = 1;
	si.si_uid = 12345;
	si.si_value.sival_int = 99;
	ATF_CHECK_EQ_MSG(0, thr_sigqueueinfo(pid, id, SIGUSR1, &si),
	    "thr_sigqueueinfo: %s", strerror(errno));
	finish_child(pid, donefd, 'w');
}

ATF_TC_WITHOUT_HEAD(child_eperm);
ATF_TC_BODY(child_eperm, tc)
{
	siginfo_t si;
	long id;
	pid_t pid;
	int donefd;

	pid = start_child(&id, &donefd);
	memset(&si, 0, sizeof(si));
	si.si_code = SI_USER;
	ATF_CHECK_ERRNO(EPERM, thr_sigqueueinfo(pid, id, SIGUSR1, &si) == -1);
	si.si_code = SI_LWP;
	ATF_CHECK_ERRNO(EPERM, thr_sigqueueinfo(pid, id, SIGUSR1, &si) == -1);

	/* The thread id of the child with the pid of the parent. */
	si.si_code = SI_QUEUE;
	ATF_CHECK_ERRNO(ESRCH,
	    thr_sigqueueinfo(getpid(), id, SIGUSR1, &si) == -1);
	finish_child(pid, donefd, 'q');
}

ATF_TC_WITHOUT_HEAD(signal_zero);
ATF_TC_BODY(signal_zero, tc)
{
	siginfo_t si;

	memset(&si, 0, sizeof(si));
	ATF_CHECK_EQ(0, thr_sigqueueinfo(getpid(), self_id(), 0, &si));
	ATF_CHECK_ERRNO(ESRCH,
	    thr_sigqueueinfo(getpid(), INT_MAX, 0, &si) == -1);
}

ATF_TC_WITHOUT_HEAD(einval);
ATF_TC_BODY(einval, tc)
{
	siginfo_t si;

	memset(&si, 0, sizeof(si));
	ATF_CHECK_ERRNO(EINVAL,
	    thr_sigqueueinfo(getpid(), self_id(), 200, &si) == -1);
	ATF_CHECK_ERRNO(EINVAL,
	    thr_sigqueueinfo(getpid(), self_id(), -1, &si) == -1);
	ATF_CHECK_ERRNO(EINVAL, thr_sigqueueinfo(0, self_id(), 0, &si) == -1);
	ATF_CHECK_ERRNO(EINVAL, thr_sigqueueinfo(getpid(), 0, 0, &si) == -1);
	ATF_CHECK_ERRNO(EINVAL, thr_sigqueueinfo(getpid(), -1, 0, &si) == -1);
}

ATF_TC_WITHOUT_HEAD(efault);
ATF_TC_BODY(efault, tc)
{
	void *p;
	long pgsz;

	pgsz = sysconf(_SC_PAGESIZE);
	p = mmap(NULL, pgsz, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_CHECK_ERRNO(EFAULT,
	    thr_sigqueueinfo(getpid(), self_id(), SIGUSR1, p) == -1);
	ATF_REQUIRE_EQ(0, munmap(p, pgsz));
}

ATF_TC_WITHOUT_HEAD(capability_mode);
ATF_TC_BODY(capability_mode, tc)
{
	siginfo_t si;
	long parent_id;
	pid_t pid;
	int status;

	if (!feature_present("security_capability_mode"))
		atf_tc_skip("Capsicum capability mode is not in the kernel");

	parent_id = self_id();
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		memset(&si, 0, sizeof(si));
		si.si_code = SI_QUEUE;
		if (cap_enter() != 0)
			_exit(1);
		if (thr_sigqueueinfo(getpid(), self_id(), 0, &si) != 0)
			_exit(2);
		if (thr_sigqueueinfo(getppid(), parent_id, 0, &si) != -1 ||
		    errno != ECAPMODE)
			_exit(3);
		_exit(0);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child status %#x", status);
}

ATF_TC_WITHOUT_HEAD(rt_queue);
ATF_TC_BODY(rt_queue, tc)
{
	struct timespec ts;
	siginfo_t got, si;
	sigset_t set;
	int i;

	block_signal(SIGRTMIN);
	for (i = 1; i <= 3; i++) {
		memset(&si, 0, sizeof(si));
		si.si_code = SI_QUEUE;
		si.si_value.sival_int = i;
		ATF_REQUIRE_EQ(0, thr_sigqueueinfo(getpid(), self_id(),
		    SIGRTMIN, &si));
	}
	sigemptyset(&set);
	sigaddset(&set, SIGRTMIN);
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	for (i = 1; i <= 3; i++) {
		ATF_REQUIRE_EQ(SIGRTMIN, sigtimedwait(&set, &got, &ts));
		ATF_CHECK_EQ(i, got.si_value.sival_int);
	}
	ATF_CHECK_ERRNO(EAGAIN, sigtimedwait(&set, &got, &ts) == -1);
}

ATF_TC_WITHOUT_HEAD(queue_limit);
ATF_TC_BODY(queue_limit, tc)
{
	struct timespec ts;
	siginfo_t got, si;
	sigset_t set;
	size_t len;
	int i, max;

	len = sizeof(max);
	ATF_REQUIRE_EQ(0, sysctlbyname("kern.sigqueue.max_pending_per_proc",
	    &max, &len, NULL, 0));
	block_signal(SIGRTMIN);
	memset(&si, 0, sizeof(si));
	si.si_code = SI_QUEUE;
	for (i = 0; i <= max; i++)
		if (thr_sigqueueinfo(getpid(), self_id(), SIGRTMIN, &si) != 0)
			break;
	ATF_CHECK_EQ(EAGAIN, errno);
	ATF_CHECK(i <= max);

	sigemptyset(&set);
	sigaddset(&set, SIGRTMIN);
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	while (sigtimedwait(&set, &got, &ts) == SIGRTMIN)
		;
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, self_any_code);
	ATF_TP_ADD_TC(tp, other_thread);
	ATF_TP_ADD_TC(tp, child_queue);
	ATF_TP_ADD_TC(tp, child_eperm);
	ATF_TP_ADD_TC(tp, signal_zero);
	ATF_TP_ADD_TC(tp, einval);
	ATF_TP_ADD_TC(tp, efault);
	ATF_TP_ADD_TC(tp, capability_mode);
	ATF_TP_ADD_TC(tp, rt_queue);
	ATF_TP_ADD_TC(tp, queue_limit);

	return (atf_no_error());
}
