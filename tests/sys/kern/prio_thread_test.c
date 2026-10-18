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
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include <atf-c.h>

/*
 * A second thread: it gives its id, waits for the test to change the nice
 * values, reads its own value and exits.
 */
static pthread_barrier_t barrier;
static long worker_id;
static int worker_nice;

static void *
worker_main(void *arg __unused)
{
	ATF_REQUIRE_EQ(0, thr_self(&worker_id));
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	worker_nice = getpriority(PRIO_THREAD, 0);
	pthread_barrier_wait(&barrier);
	return (NULL);
}

static void
worker_start(pthread_t *thr)
{
	ATF_REQUIRE_EQ(0, pthread_barrier_init(&barrier, NULL, 2));
	ATF_REQUIRE_EQ(0, pthread_create(thr, NULL, worker_main, NULL));
	pthread_barrier_wait(&barrier);
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

static void
require_nice_zero(void)
{
	errno = 0;
	ATF_REQUIRE_EQ_MSG(0, getpriority(PRIO_PROCESS, 0),
	    "the test needs a nice value of 0");
	ATF_REQUIRE_EQ(0, errno);
}

ATF_TC_WITHOUT_HEAD(self);
ATF_TC_BODY(self, tc)
{
	require_nice_zero();
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, 5));
	ATF_CHECK_EQ(5, getpriority(PRIO_THREAD, 0));
	ATF_CHECK_EQ(5, getpriority(PRIO_THREAD, self_id()));
	/* The value of the process does not change. */
	ATF_CHECK_EQ(0, getpriority(PRIO_PROCESS, 0));
}

ATF_TC_WITHOUT_HEAD(other_thread);
ATF_TC_BODY(other_thread, tc)
{
	pthread_t thr;

	require_nice_zero();
	worker_start(&thr);
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, worker_id, 7));
	ATF_CHECK_EQ(7, getpriority(PRIO_THREAD, worker_id));
	ATF_CHECK_EQ(0, getpriority(PRIO_THREAD, 0));
	worker_finish(thr);
	ATF_CHECK_EQ(7, worker_nice);
}

ATF_TC_WITHOUT_HEAD(process_id);
ATF_TC_BODY(process_id, tc)
{
	pthread_t thr;

	/* A process id names the first thread of the process. */
	require_nice_zero();
	worker_start(&thr);
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, getpid(), 3));
	ATF_CHECK_EQ(3, getpriority(PRIO_THREAD, getpid()));
	ATF_CHECK_EQ(3, getpriority(PRIO_THREAD, 0));
	ATF_CHECK_EQ(0, getpriority(PRIO_THREAD, worker_id));
	worker_finish(thr);
	ATF_CHECK_EQ(0, worker_nice);
}

ATF_TC_WITHOUT_HEAD(process_sets_all);
ATF_TC_BODY(process_sets_all, tc)
{
	pthread_t thr;

	require_nice_zero();
	worker_start(&thr);
	ATF_REQUIRE_EQ(0, setpriority(PRIO_PROCESS, 0, 4));
	ATF_CHECK_EQ(4, getpriority(PRIO_PROCESS, 0));
	ATF_CHECK_EQ(4, getpriority(PRIO_THREAD, 0));
	ATF_CHECK_EQ(4, getpriority(PRIO_THREAD, worker_id));
	worker_finish(thr);
	ATF_CHECK_EQ(4, worker_nice);
}

ATF_TC(eacces);
ATF_TC_HEAD(eacces, tc)
{
	atf_tc_set_md_var(tc, "require.user", "unprivileged");
}
ATF_TC_BODY(eacces, tc)
{
	pthread_t thr;

	require_nice_zero();
	worker_start(&thr);
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, worker_id, 10));
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, 5));
	/* A lower value for one thread needs the privilege. */
	ATF_CHECK_ERRNO(EACCES, setpriority(PRIO_THREAD, 0, 4) == -1);
	ATF_CHECK_ERRNO(EACCES, setpriority(PRIO_THREAD, worker_id, 5) == -1);
	/* So does a value for the process below the value of a thread. */
	ATF_CHECK_ERRNO(EACCES, setpriority(PRIO_PROCESS, 0, 7) == -1);
	ATF_CHECK_EQ(10, getpriority(PRIO_THREAD, worker_id));
	ATF_CHECK_EQ(5, getpriority(PRIO_THREAD, 0));
	/* A value at or above the value of each thread is allowed. */
	ATF_CHECK_EQ(0, setpriority(PRIO_PROCESS, 0, 10));
	ATF_CHECK_EQ(10, getpriority(PRIO_THREAD, 0));
	worker_finish(thr);
	ATF_CHECK_EQ(10, worker_nice);
}

ATF_TC(root_lower);
ATF_TC_HEAD(root_lower, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(root_lower, tc)
{
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, 10));
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, -5));
	ATF_CHECK_EQ(-5, getpriority(PRIO_THREAD, 0));
	/* Values outside the range are clamped. */
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, -100));
	ATF_CHECK_EQ(PRIO_MIN, getpriority(PRIO_THREAD, 0));
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, 100));
	ATF_CHECK_EQ(PRIO_MAX, getpriority(PRIO_THREAD, 0));
}

ATF_TC_WITHOUT_HEAD(esrch);
ATF_TC_BODY(esrch, tc)
{
	/*
	 * No thread has the largest id.  A joined thread is not a stable
	 * case: the kernel can give its id to a new thread of any process.
	 */
	ATF_CHECK_ERRNO(ESRCH, setpriority(PRIO_THREAD, INT_MAX, 1) == -1);
	errno = 0;
	ATF_CHECK_EQ(-1, getpriority(PRIO_THREAD, INT_MAX));
	ATF_CHECK_EQ(ESRCH, errno);
	ATF_CHECK_ERRNO(ESRCH, setpriority(PRIO_THREAD, -1, 1) == -1);
}

ATF_TC(eperm);
ATF_TC_HEAD(eperm, tc)
{
	atf_tc_set_md_var(tc, "require.user", "unprivileged");
}
ATF_TC_BODY(eperm, tc)
{
	/* The first thread of init. */
	ATF_CHECK_ERRNO(EPERM, setpriority(PRIO_THREAD, 1, 10) == -1);
}

ATF_TC_WITHOUT_HEAD(inherit);
ATF_TC_BODY(inherit, tc)
{
	pthread_t thr;
	pid_t pid;
	int status;

	require_nice_zero();
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, 0, 6));
	worker_start(&thr);
	ATF_CHECK_EQ(6, getpriority(PRIO_THREAD, worker_id));
	worker_finish(thr);
	ATF_CHECK_EQ(6, worker_nice);

	/* A new process gets the value of the thread and of the process. */
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		_exit(getpriority(PRIO_THREAD, 0) == 6 &&
		    getpriority(PRIO_PROCESS, 0) == 0 ? 0 : 1);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* The ki_nice of thread tid, or of the process when tid is 0. */
static int
kinfo_nice(long tid)
{
	struct kinfo_proc *kp;
	size_t i, len;
	int mib[4], nice;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID | (tid != 0 ? KERN_PROC_INC_THREAD : 0);
	mib[3] = getpid();
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), NULL, &len, NULL, 0));
	kp = malloc(len);
	ATF_REQUIRE(kp != NULL);
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), kp, &len, NULL, 0));
	nice = INT_MAX;
	for (i = 0; i < len / sizeof(*kp); i++) {
		if (tid == 0 || kp[i].ki_tid == tid)
			nice = kp[i].ki_nice;
	}
	free(kp);
	ATF_REQUIRE(nice != INT_MAX);
	return (nice);
}

ATF_TC_WITHOUT_HEAD(kinfo);
ATF_TC_BODY(kinfo, tc)
{
	pthread_t thr;

	require_nice_zero();
	worker_start(&thr);
	ATF_REQUIRE_EQ(0, setpriority(PRIO_THREAD, worker_id, 9));
	ATF_CHECK_EQ(9, kinfo_nice(worker_id));
	ATF_CHECK_EQ(0, kinfo_nice(self_id()));
	/* Without KERN_PROC_INC_THREAD, ki_nice is the value of the process. */
	ATF_CHECK_EQ(0, kinfo_nice(0));
	worker_finish(thr);
}

ATF_TC_WITHOUT_HEAD(einval);
ATF_TC_BODY(einval, tc)
{
	ATF_CHECK_ERRNO(EINVAL, setpriority(PRIO_THREAD + 1, 0, 0) == -1);
	ATF_CHECK_ERRNO(EINVAL, setpriority(-1, 0, 0) == -1);
	errno = 0;
	ATF_CHECK_EQ(-1, getpriority(PRIO_THREAD + 1, 0));
	ATF_CHECK_EQ(EINVAL, errno);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, self);
	ATF_TP_ADD_TC(tp, other_thread);
	ATF_TP_ADD_TC(tp, process_id);
	ATF_TP_ADD_TC(tp, process_sets_all);
	ATF_TP_ADD_TC(tp, eacces);
	ATF_TP_ADD_TC(tp, root_lower);
	ATF_TP_ADD_TC(tp, esrch);
	ATF_TP_ADD_TC(tp, eperm);
	ATF_TP_ADD_TC(tp, inherit);
	ATF_TP_ADD_TC(tp, kinfo);
	ATF_TP_ADD_TC(tp, einval);

	return (atf_no_error());
}
