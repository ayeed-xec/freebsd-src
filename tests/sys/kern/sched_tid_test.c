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
#include <sys/resource.h>
#include <sys/rtprio.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <atf-c.h>

/*
 * A second thread: it gives its id, waits for the test, reads its own
 * policy, priority and nice value, and exits.
 */
static pthread_barrier_t barrier;
static long worker_id;
static int worker_policy;
static int worker_prio;
static int worker_nice;

static void *
worker_main(void *arg __unused)
{
	struct sched_param p;

	ATF_REQUIRE_EQ(0, thr_self(&worker_id));
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	worker_policy = sched_getscheduler(0);
	worker_prio = sched_getparam(0, &p) == 0 ? p.sched_priority : -1;
	worker_nice = getpriority(PRIO_THREAD, 0);
	pthread_barrier_wait(&barrier);
	return (NULL);
}

static void
worker_start_attr(pthread_t *thr, const pthread_attr_t *attr)
{
	ATF_REQUIRE_EQ(0, pthread_barrier_init(&barrier, NULL, 2));
	ATF_REQUIRE_EQ(0, pthread_create(thr, attr, worker_main, NULL));
	pthread_barrier_wait(&barrier);
}

static void
worker_start(pthread_t *thr)
{
	worker_start_attr(thr, NULL);
}

static void
worker_finish(pthread_t thr)
{
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	ATF_REQUIRE_EQ(0, pthread_join(thr, NULL));
	ATF_REQUIRE_EQ(0, pthread_barrier_destroy(&barrier));
}

static long
self_id(void)
{
	long id;

	ATF_REQUIRE_EQ(0, thr_self(&id));
	return (id);
}

static struct sched_param
prio(int policy, int offset)
{
	struct sched_param p;

	p.sched_priority = sched_get_priority_min(policy) + offset;
	return (p);
}

static void
set_policy(pid_t id, int policy, int offset)
{
	struct sched_param p;

	p = prio(policy & ~SCHED_RESET_ON_FORK, offset);
	ATF_REQUIRE_MSG(sched_setscheduler(id, policy, &p) == 0,
	    "sched_setscheduler(%d, %#x): %d", id, policy, errno);
}

static int
get_prio(pid_t id)
{
	struct sched_param p;

	ATF_REQUIRE_EQ(0, sched_getparam(id, &p));
	return (p.sched_priority);
}

/* The id of the first thread of process pid. */
static long
first_thread_id(pid_t pid)
{
	struct kinfo_proc *kp;
	size_t len;
	int mib[4];
	long tid;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID | KERN_PROC_INC_THREAD;
	mib[3] = pid;
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), NULL, &len, NULL, 0));
	kp = malloc(len);
	ATF_REQUIRE(kp != NULL);
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), kp, &len, NULL, 0));
	ATF_REQUIRE(len >= sizeof(*kp));
	tid = kp[0].ki_tid;
	free(kp);
	return (tid);
}

ATF_TC(tid_set_get);
ATF_TC_HEAD(tid_set_get, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(tid_set_get, tc)
{
	struct timespec ts, ts0;
	struct sched_param p;
	pthread_t thr;

	worker_start(&thr);
	set_policy(worker_id, SCHED_FIFO, 1);
	ATF_CHECK_EQ(SCHED_FIFO, sched_getscheduler(worker_id));
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(0));
	ATF_CHECK_EQ(prio(SCHED_FIFO, 1).sched_priority, get_prio(worker_id));
	p = prio(SCHED_FIFO, 2);
	ATF_REQUIRE_EQ(0, sched_setparam(worker_id, &p));
	ATF_CHECK_EQ(p.sched_priority, get_prio(worker_id));
	ATF_CHECK_EQ(SCHED_FIFO, sched_getscheduler(worker_id));
	ATF_REQUIRE_EQ(0, sched_rr_get_interval(worker_id, &ts));
	ATF_REQUIRE_EQ(0, sched_rr_get_interval(0, &ts0));
	ATF_CHECK(ts.tv_sec == ts0.tv_sec && ts.tv_nsec == ts0.tv_nsec);
	worker_finish(thr);
	ATF_CHECK_EQ(SCHED_FIFO, worker_policy);
	ATF_CHECK_EQ(p.sched_priority, worker_prio);
}

ATF_TC_WITHOUT_HEAD(tid_get);
ATF_TC_BODY(tid_get, tc)
{
	struct sched_param p;
	struct timespec ts;
	pthread_t thr;

	/* The getters need no privilege. */
	worker_start(&thr);
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(worker_id));
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(self_id()));
	ATF_CHECK_EQ(0, sched_getparam(worker_id, &p));
	ATF_CHECK_EQ(0, sched_rr_get_interval(worker_id, &ts));
	worker_finish(thr);
}

ATF_TC(process_id);
ATF_TC_HEAD(process_id, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(process_id, tc)
{
	pthread_t thr;

	/* A process id names the first thread of the process. */
	worker_start(&thr);
	set_policy(getpid(), SCHED_RR, 3);
	ATF_CHECK_EQ(SCHED_RR, sched_getscheduler(0));
	ATF_CHECK_EQ(SCHED_RR, sched_getscheduler(getpid()));
	ATF_CHECK_EQ(SCHED_RR, sched_getscheduler(self_id()));
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(worker_id));
	worker_finish(thr);
	ATF_CHECK_EQ(SCHED_OTHER, worker_policy);
}

ATF_TC(other_process);
ATF_TC_HEAD(other_process, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(other_process, tc)
{
	long tid;
	pid_t pid;
	int go[2], idp[2], status;
	char c;

	ATF_REQUIRE_EQ(0, pipe(go));
	ATF_REQUIRE_EQ(0, pipe(idp));
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		if (thr_self(&tid) != 0 ||
		    write(idp[1], &tid, sizeof(tid)) != sizeof(tid) ||
		    read(go[0], &c, 1) != 1)
			_exit(2);
		_exit(sched_getscheduler(0) ==
		    (SCHED_RR | SCHED_RESET_ON_FORK) ? 0 : 1);
	}
	ATF_REQUIRE_EQ(sizeof(tid), read(idp[0], &tid, sizeof(tid)));
	set_policy(tid, SCHED_RR | SCHED_RESET_ON_FORK, 2);
	ATF_CHECK_EQ(SCHED_RR | SCHED_RESET_ON_FORK, sched_getscheduler(tid));
	ATF_CHECK_EQ(SCHED_RR | SCHED_RESET_ON_FORK, sched_getscheduler(pid));
	ATF_CHECK_EQ(prio(SCHED_RR, 2).sched_priority, get_prio(pid));
	/* The calling thread does not change. */
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(0));
	ATF_REQUIRE_EQ(1, write(go[1], "g", 1));
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

ATF_TC_WITHOUT_HEAD(esrch);
ATF_TC_BODY(esrch, tc)
{
	struct sched_param p;
	struct timespec ts;
	pid_t ids[] = { INT_MAX, -1 };
	size_t i;

	/*
	 * No thread has the largest id.  A joined thread is not a stable
	 * case: pthread_join() can return before thr_exit(2) removes the
	 * id, and the kernel can give the id to a new thread later.
	 */
	p = prio(SCHED_OTHER, 0);
	for (i = 0; i < nitems(ids); i++) {
		ATF_CHECK_ERRNO(ESRCH,
		    sched_setscheduler(ids[i], SCHED_OTHER, &p) == -1);
		ATF_CHECK_ERRNO(ESRCH, sched_getscheduler(ids[i]) == -1);
		ATF_CHECK_ERRNO(ESRCH, sched_setparam(ids[i], &p) == -1);
		ATF_CHECK_ERRNO(ESRCH, sched_getparam(ids[i], &p) == -1);
		ATF_CHECK_ERRNO(ESRCH,
		    sched_rr_get_interval(ids[i], &ts) == -1);
	}
}

ATF_TC(eperm);
ATF_TC_HEAD(eperm, tc)
{
	atf_tc_set_md_var(tc, "require.user", "unprivileged");
}
ATF_TC_BODY(eperm, tc)
{
	struct sched_param p;
	long tid;

	/* The first thread of init, by its thread id. */
	tid = first_thread_id(1);
	p = prio(SCHED_OTHER, 0);
	ATF_CHECK_ERRNO(EPERM, sched_setparam(tid, &p) == -1);
	ATF_CHECK_ERRNO(EPERM, sched_setscheduler(tid, SCHED_OTHER, &p) == -1);
	/* The policy needs the privilege for the calling thread too. */
	ATF_CHECK_ERRNO(EPERM, sched_setscheduler(0, SCHED_OTHER, &p) == -1);
	ATF_CHECK_ERRNO(EPERM, sched_setscheduler(0,
	    SCHED_OTHER | SCHED_RESET_ON_FORK, &p) == -1);
}

ATF_TC(reset_flag);
ATF_TC_HEAD(reset_flag, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(reset_flag, tc)
{
	set_policy(0, SCHED_FIFO | SCHED_RESET_ON_FORK, 0);
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK, sched_getscheduler(0));
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK,
	    sched_getscheduler(self_id()));
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK,
	    sched_getscheduler(getpid()));
	ATF_CHECK_EQ(prio(SCHED_FIFO, 0).sched_priority, get_prio(0));
	/* The flag also goes with SCHED_OTHER. */
	set_policy(0, SCHED_OTHER | SCHED_RESET_ON_FORK, 0);
	ATF_CHECK_EQ(SCHED_OTHER | SCHED_RESET_ON_FORK, sched_getscheduler(0));
}

ATF_TC(flag_kept);
ATF_TC_HEAD(flag_kept, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(flag_kept, tc)
{
	struct sched_param p;
	struct rtprio rtp;

	set_policy(0, SCHED_FIFO | SCHED_RESET_ON_FORK, 0);
	p = prio(SCHED_FIFO, 4);
	ATF_REQUIRE_EQ(0, sched_setparam(0, &p));
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK, sched_getscheduler(0));
	ATF_CHECK_EQ(p.sched_priority, get_prio(0));
	rtp.type = RTP_PRIO_REALTIME;
	rtp.prio = 10;
	ATF_REQUIRE_EQ(0, rtprio_thread(RTP_SET, 0, &rtp));
	ATF_CHECK_EQ(SCHED_RR | SCHED_RESET_ON_FORK, sched_getscheduler(0));
	rtp.type = RTP_PRIO_NORMAL;
	rtp.prio = 0;
	ATF_REQUIRE_EQ(0, rtprio(RTP_SET, 0, &rtp));
	ATF_CHECK_EQ(SCHED_OTHER | SCHED_RESET_ON_FORK, sched_getscheduler(0));
}

ATF_TC(flag_cleared);
ATF_TC_HEAD(flag_cleared, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(flag_cleared, tc)
{
	set_policy(0, SCHED_FIFO | SCHED_RESET_ON_FORK, 0);
	set_policy(0, SCHED_FIFO, 0);
	ATF_CHECK_EQ(SCHED_FIFO, sched_getscheduler(0));
	set_policy(0, SCHED_RR | SCHED_RESET_ON_FORK, 0);
	set_policy(0, SCHED_OTHER, 0);
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(0));
}

ATF_TC(einval);
ATF_TC_HEAD(einval, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(einval, tc)
{
	struct sched_param p;

	set_policy(0, SCHED_RR | SCHED_RESET_ON_FORK, 0);
	p = prio(SCHED_OTHER, 0);
	ATF_CHECK_ERRNO(EINVAL,
	    sched_setscheduler(0, SCHED_RESET_ON_FORK, &p) == -1);
	ATF_CHECK_ERRNO(EINVAL,
	    sched_setscheduler(0, 0x7f | SCHED_RESET_ON_FORK, &p) == -1);
	ATF_CHECK_ERRNO(EINVAL, sched_setscheduler(0, 0x20000000 | SCHED_FIFO,
	    &p) == -1);
	p.sched_priority = sched_get_priority_max(SCHED_FIFO) + 1;
	ATF_CHECK_ERRNO(EINVAL,
	    sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &p) == -1);
	/* A failed call changes neither the policy nor the flag. */
	ATF_CHECK_EQ(SCHED_RR | SCHED_RESET_ON_FORK, sched_getscheduler(0));
}

ATF_TC(fork_reset);
ATF_TC_HEAD(fork_reset, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(fork_reset, tc)
{
	pid_t pid;
	int status;

	ATF_REQUIRE_EQ(0, setpriority(PRIO_PROCESS, 0, -5));
	set_policy(0, SCHED_FIFO | SCHED_RESET_ON_FORK, 0);
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		_exit(sched_getscheduler(0) == SCHED_OTHER &&
		    getpriority(PRIO_THREAD, 0) == 0 &&
		    getpriority(PRIO_PROCESS, 0) == 0 ? 0 : 1);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	/* The parent keeps all of it. */
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK, sched_getscheduler(0));
	ATF_CHECK_EQ(-5, getpriority(PRIO_THREAD, 0));
	ATF_CHECK_EQ(-5, getpriority(PRIO_PROCESS, 0));
}

ATF_TC(fork_reset_nice);
ATF_TC_HEAD(fork_reset_nice, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(fork_reset_nice, tc)
{
	pid_t pid;
	int status;

	/* A time sharing thread passes on only a nice value of at least 0. */
	ATF_REQUIRE_EQ(0, setpriority(PRIO_PROCESS, 0, -3));
	set_policy(0, SCHED_OTHER | SCHED_RESET_ON_FORK, 0);
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		_exit(sched_getscheduler(0) == SCHED_OTHER &&
		    getpriority(PRIO_THREAD, 0) == 0 &&
		    getpriority(PRIO_PROCESS, 0) == 0 ? 0 : 1);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/* A positive nice value stays. */
	ATF_REQUIRE_EQ(0, setpriority(PRIO_PROCESS, 0, 4));
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		_exit(getpriority(PRIO_THREAD, 0) == 4 &&
		    getpriority(PRIO_PROCESS, 0) == 4 ? 0 : 1);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

ATF_TC(thread_reset);
ATF_TC_HEAD(thread_reset, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(thread_reset, tc)
{
	pthread_t thr;

	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, -5));
	set_policy(0, SCHED_FIFO | SCHED_RESET_ON_FORK, 0);
	worker_start(&thr);
	ATF_CHECK_EQ(SCHED_OTHER, sched_getscheduler(worker_id));
	ATF_CHECK_EQ(0, getpriority(PRIO_THREAD, worker_id));
	worker_finish(thr);
	ATF_CHECK_EQ(SCHED_OTHER, worker_policy);
	ATF_CHECK_EQ(0, worker_nice);
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK, sched_getscheduler(0));
	ATF_CHECK_EQ(-5, getpriority(PRIO_THREAD, 0));
}

ATF_TC(thread_explicit);
ATF_TC_HEAD(thread_explicit, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(thread_explicit, tc)
{
	struct sched_param p;
	pthread_attr_t attr;
	pthread_t thr;

	/* An explicit attribute of the new thread still applies. */
	set_policy(0, SCHED_FIFO | SCHED_RESET_ON_FORK, 0);
	p = prio(SCHED_RR, 5);
	ATF_REQUIRE_EQ(0, pthread_attr_init(&attr));
	ATF_REQUIRE_EQ(0, pthread_attr_setinheritsched(&attr,
	    PTHREAD_EXPLICIT_SCHED));
	ATF_REQUIRE_EQ(0, pthread_attr_setschedpolicy(&attr, SCHED_RR));
	ATF_REQUIRE_EQ(0, pthread_attr_setschedparam(&attr, &p));
	worker_start_attr(&thr, &attr);
	ATF_CHECK_EQ(SCHED_RR, sched_getscheduler(worker_id));
	worker_finish(thr);
	ATF_REQUIRE_EQ(0, pthread_attr_destroy(&attr));
	ATF_CHECK_EQ(SCHED_RR, worker_policy);
	ATF_CHECK_EQ(p.sched_priority, worker_prio);
}

ATF_TC(no_flag_inherits);
ATF_TC_HEAD(no_flag_inherits, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(no_flag_inherits, tc)
{
	pthread_t thr;
	pid_t pid;
	int status;

	/* Without the flag, new threads and processes inherit, as before. */
	ATF_REQUIRE_EQ(0, setpriority(PRIO_PROCESS, 0, -2));
	set_policy(0, SCHED_FIFO, 3);
	worker_start(&thr);
	worker_finish(thr);
	ATF_CHECK_EQ(SCHED_FIFO, worker_policy);
	ATF_CHECK_EQ(prio(SCHED_FIFO, 3).sched_priority, worker_prio);
	ATF_CHECK_EQ(-2, worker_nice);
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		_exit(sched_getscheduler(0) == SCHED_FIFO &&
		    getpriority(PRIO_THREAD, 0) == -2 &&
		    getpriority(PRIO_PROCESS, 0) == -2 ? 0 : 1);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

ATF_TC(exec_keeps_flag);
ATF_TC_HEAD(exec_keeps_flag, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(exec_keeps_flag, tc)
{
	struct sched_param p;
	pid_t pid;
	int fds[2], status;
	char c;

	/* The write end closes at execve(2), so EOF means the exec is done. */
	ATF_REQUIRE_EQ(0, pipe2(fds, O_CLOEXEC));
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		p = prio(SCHED_FIFO, 0);
		if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK,
		    &p) != 0) {
			(void)write(fds[1], "F", 1);
			_exit(1);
		}
		execl("/bin/sleep", "sleep", "60", (char *)NULL);
		_exit(127);
	}
	ATF_REQUIRE_EQ(0, close(fds[1]));
	ATF_REQUIRE_EQ(0, read(fds[0], &c, 1));
	ATF_REQUIRE_EQ_MSG(0, waitpid(pid, &status, WNOHANG),
	    "the child did not exec");
	ATF_CHECK_EQ(SCHED_FIFO | SCHED_RESET_ON_FORK, sched_getscheduler(pid));
	ATF_REQUIRE_EQ(0, kill(pid, SIGKILL));
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tid_set_get);
	ATF_TP_ADD_TC(tp, tid_get);
	ATF_TP_ADD_TC(tp, process_id);
	ATF_TP_ADD_TC(tp, other_process);
	ATF_TP_ADD_TC(tp, esrch);
	ATF_TP_ADD_TC(tp, eperm);
	ATF_TP_ADD_TC(tp, reset_flag);
	ATF_TP_ADD_TC(tp, flag_kept);
	ATF_TP_ADD_TC(tp, flag_cleared);
	ATF_TP_ADD_TC(tp, einval);
	ATF_TP_ADD_TC(tp, fork_reset);
	ATF_TP_ADD_TC(tp, fork_reset_nice);
	ATF_TP_ADD_TC(tp, thread_reset);
	ATF_TP_ADD_TC(tp, thread_explicit);
	ATF_TP_ADD_TC(tp, no_flag_inherits);
	ATF_TP_ADD_TC(tp, exec_keeps_flag);

	return (atf_no_error());
}
