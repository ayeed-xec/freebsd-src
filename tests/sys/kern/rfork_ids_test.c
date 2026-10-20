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
#include <sys/procdesc.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check_child(pid_t, int);
static void check_ids(int);
static void fault_handler(int, siginfo_t *, void *);
static void *fork_worker(void *);
#if defined(__aarch64__)
static void stack_child(struct rfork_ids *);
static pid_t stack_fork(int, struct rfork_ids *, void *,
    void (*)(struct rfork_ids *)) __attribute__((naked));
#endif

static void *fault_address;
/* Resolve child calls before a raw fork can inherit the rtld lock. */
static pid_t (*volatile child_getpid)(void) = getpid;
static void (*volatile child_exit)(int) = _exit;

#if defined(__aarch64__)
/* Switch stacks before a shared-memory child calls any C code. */
static pid_t
stack_fork(int flags __unused, struct rfork_ids *ids __unused,
    void *stack __unused, void (*fn)(struct rfork_ids *) __unused)
{
	__asm__ volatile(
	    "mov x9, x2\n"
	    "mov x10, x3\n"
	    "mov x11, x1\n"
	    "mov x8, " __XSTRING(SYS_rfork_ids) "\n"
	    "svc #0\n"
	    "b.cs 2f\n"
	    "cbnz x0, 1f\n"
	    "mov sp, x9\n"
	    "mov x0, x11\n"
	    "mov x29, xzr\n"
	    "br x10\n"
	    "1: ret\n"
	    "2: neg x0, x0\n"
	    "ret\n");
}
#endif

static void
check_child(pid_t pid, int options)
{
	int status;

	ATF_REQUIRE_MSG(pid > 0, "fork: %s", strerror(errno));
	ATF_REQUIRE_EQ(waitpid(pid, &status, options), pid);
	ATF_REQUIRE(WIFEXITED(status));
	ATF_REQUIRE_EQ(WEXITSTATUS(status), 0);
}

static void
check_ids(int flags)
{
	struct rfork_ids ids, observed;
	long tid;
	pid_t pid;
	int fd[2];

	ATF_REQUIRE_EQ(pipe(fd), 0);
	ids.ri_pid = -1;
	ids.ri_tid = -1;
	pid = rfork_ids(flags, &ids);
	if (pid == 0) {
		if (thr_self(&tid) != 0 || ids.ri_pid != getpid() ||
		    ids.ri_tid != tid || ids.ri_tid <= 0)
			_exit(1);
		if (write(fd[1], &ids, sizeof(ids)) != sizeof(ids))
			_exit(2);
		_exit(0);
	}
	ATF_REQUIRE_MSG(pid > 0, "rfork_ids: %s", strerror(errno));
	ATF_REQUIRE_EQ(close(fd[1]), 0);
	ATF_REQUIRE_EQ(read(fd[0], &observed, sizeof(observed)),
	    sizeof(observed));
	ATF_REQUIRE_EQ(ids.ri_pid, pid);
	ATF_REQUIRE_EQ(ids.ri_tid, observed.ri_tid);
	ATF_REQUIRE_EQ(ids.ri_pid, observed.ri_pid);
	check_child(pid, (flags & RFTSIGZMB) != 0 ? WLINUXCLONE : 0);
	ATF_REQUIRE_EQ(ids.ri_pid, pid);
	ATF_REQUIRE_EQ(ids.ri_tid, observed.ri_tid);
	ATF_REQUIRE_EQ(close(fd[0]), 0);
}

ATF_TC(ids);
ATF_TC_HEAD(ids, tc)
{
	atf_tc_set_md_var(tc, "descr", "Both processes receive the child ids");
}
ATF_TC_BODY(ids, tc)
{
	check_ids(RFPROC | RFFDG);
}

ATF_TC(flags);
ATF_TC_HEAD(flags, tc)
{
	atf_tc_set_md_var(tc, "descr", "Invalid flags do not create a process");
}
ATF_TC_BODY(flags, tc)
{
	const int invalid[] = { 0, RFFDG, RFPROC | RFSTOPPED,
	    RFPROC | RFHIGHPID, RFPROC | RFPROCDESC, RFPROC | RFSPAWN,
	    RFPROC | RFFDG | RFCFDG, RFPROC | RFNAMEG,
	    RFPROC | RFTSIGFLAGS(SIGUSR1),
	    RFPROC | RFTSIGZMB | RFTSIGFLAGS(255) };
	struct rfork_ids ids;
	size_t i;

	for (i = 0; i < nitems(invalid); i++) {
		ATF_REQUIRE_ERRNO(EINVAL, rfork_ids(invalid[i], &ids) == -1);
		ATF_REQUIRE_ERRNO(ECHILD, waitpid(-1, NULL, WNOHANG) == -1);
	}
}

ATF_TC(address);
ATF_TC_HEAD(address, tc)
{
	atf_tc_set_md_var(tc, "descr", "Bad output storage creates no child");
}
ATF_TC_BODY(address, tc)
{
	void *p;
	size_t len;

	len = getpagesize();
	p = mmap(NULL, len, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_REQUIRE_ERRNO(EFAULT, rfork_ids(RFPROC | RFFDG, p) == -1);
	ATF_REQUIRE_EQ(munmap(p, len), 0);
	ATF_REQUIRE_ERRNO(EFAULT, rfork_ids(RFPROC | RFFDG, p) == -1);
	ATF_REQUIRE_ERRNO(EFAULT, rfork_ids(RFPROC | RFFDG, NULL) == -1);
	ATF_REQUIRE_ERRNO(ECHILD, waitpid(-1, NULL, WNOHANG) == -1);
}

static void
fault_handler(int sig, siginfo_t *info, void *context __unused)
{
	_exit(sig == SIGSEGV && info->si_code == SEGV_MAPERR &&
	    info->si_addr == fault_address ? 0 : 1);
}

ATF_TC(child_fault);
ATF_TC_HEAD(child_fault, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "An uninherited page faults in the child");
}
ATF_TC_BODY(child_fault, tc)
{
	struct sigaction sa;
	struct rfork_ids *ids;
	size_t len;
	pid_t pid;

	len = getpagesize();
	ids = mmap(NULL, len, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(ids != MAP_FAILED);
	ATF_REQUIRE_EQ(minherit(ids, len, INHERIT_NONE), 0);
	fault_address = ids;
	sa = (struct sigaction){ .sa_sigaction = fault_handler,
	    .sa_flags = SA_SIGINFO };
	ATF_REQUIRE_EQ(sigemptyset(&sa.sa_mask), 0);
	ATF_REQUIRE_EQ(sigaction(SIGSEGV, &sa, NULL), 0);
	pid = rfork_ids(RFPROC | RFFDG, ids);
	if (pid == 0)
		_exit(2);
	check_child(pid, 0);
	ATF_REQUIRE_EQ(ids->ri_pid, pid);
	ATF_REQUIRE(ids->ri_tid > 0);
	ATF_REQUIRE_EQ(munmap(ids, len), 0);
}

ATF_TC(signal);
ATF_TC_HEAD(signal, tc)
{
	atf_tc_set_md_var(tc, "descr", "Alternate and zero child exit signals");
}
ATF_TC_BODY(signal, tc)
{
	ATF_REQUIRE(signal(SIGUSR1, SIG_IGN) != SIG_ERR);
	check_ids(RFPROC | RFFDG | RFTSIGZMB | RFTSIGFLAGS(SIGUSR1));
	check_ids(RFPROC | RFFDG | RFTSIGZMB);
}

ATF_TC(capsicum);
ATF_TC_HEAD(capsicum, tc)
{
	atf_tc_set_md_var(tc, "descr", "The call works in capability mode");
}
ATF_TC_BODY(capsicum, tc)
{
	ATF_REQUIRE_EQ(cap_enter(), 0);
	check_ids(RFPROC | RFFDG);
}

#if defined(__aarch64__)
static void
stack_child(struct rfork_ids *ids)
{
	long tid;

	_exit(thr_self(&tid) == 0 && ids->ri_pid == getpid() &&
	    ids->ri_tid == tid ? 0 : 1);
}
#endif

ATF_TC(shared);
ATF_TC_HEAD(shared, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Shared memory and RFSPAWN store both ids");
}
ATF_TC_BODY(shared, tc)
{
#if defined(__aarch64__)
	struct rfork_ids ids;
	void *stack;
	size_t len;
	pid_t pid;
	int flags, i;

	len = 1024 * 1024;
	stack = mmap(NULL, len, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(stack != MAP_FAILED);
	for (i = 0; i < 2; i++) {
		flags = i == 0 ? RFPROC | RFFDG | RFMEM : RFSPAWN;
		pid = stack_fork(flags, &ids, (char *)stack + len, stack_child);
		check_child(pid, 0);
		ATF_REQUIRE_EQ(ids.ri_pid, pid);
		ATF_REQUIRE(ids.ri_tid > 0);
	}
	ATF_REQUIRE_EQ(munmap(stack, len), 0);
#else
	atf_tc_skip("The separate child stack helper requires aarch64");
#endif
}

ATF_TC(limit);
ATF_TC_HEAD(limit, tc)
{
	atf_tc_set_md_var(tc, "descr", "The process limit gives EAGAIN");
	atf_tc_set_md_var(tc, "require.user", "unprivileged");
}
ATF_TC_BODY(limit, tc)
{
	struct rfork_ids ids;
	struct rlimit lim;

	ATF_REQUIRE_EQ(getrlimit(RLIMIT_NPROC, &lim), 0);
	lim.rlim_cur = 0;
	ATF_REQUIRE_EQ(setrlimit(RLIMIT_NPROC, &lim), 0);
	ATF_REQUIRE_ERRNO(EAGAIN, rfork_ids(RFPROC | RFFDG, &ids) == -1);
	ATF_REQUIRE_ERRNO(ECHILD, waitpid(-1, NULL, WNOHANG) == -1);
}

ATF_TC_WITH_CLEANUP(memory);
ATF_TC_HEAD(memory, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "The swap reservation limit gives ENOMEM");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.config", "allow_sysctl_side_effects");
}
ATF_TC_BODY(memory, tc)
{
	struct rfork_ids ids;
	struct rlimit lim;
	FILE *fp;
	size_t len;
	pid_t pid;
	int old, value;

	len = sizeof(old);
	ATF_REQUIRE_EQ(sysctlbyname("vm.overcommit", &old, &len, NULL, 0), 0);
	fp = fopen("overcommit", "w");
	ATF_REQUIRE(fp != NULL);
	ATF_REQUIRE(fprintf(fp, "%d\n", old) > 0);
	ATF_REQUIRE_EQ(fclose(fp), 0);
	value = old | 2; /* Enforce RLIMIT_SWAP. */
	ATF_REQUIRE_EQ(sysctlbyname("vm.overcommit", NULL, NULL,
	    &value, sizeof(value)), 0);
	pid = fork();
	if (pid == 0) {
		lim = (struct rlimit){ 0, 0 };
		if (setrlimit(RLIMIT_SWAP, &lim) != 0 || setuid(65534) != 0)
			_exit(1);
		_exit(rfork_ids(RFPROC | RFFDG, &ids) == -1 &&
		    errno == ENOMEM ? 0 : 2);
	}
	check_child(pid, 0);
}
ATF_TC_CLEANUP(memory, tc)
{
	FILE *fp;
	int old;

	fp = fopen("overcommit", "r");
	if (fp != NULL) {
		if (fscanf(fp, "%d", &old) == 1)
			ATF_CHECK_EQ(sysctlbyname("vm.overcommit", NULL, NULL,
			    &old, sizeof(old)), 0);
		fclose(fp);
	}
}

static void *
fork_worker(void *arg __unused)
{
	struct rfork_ids ids;
	pid_t pid;
	int i, status;

	for (i = 0; i < 100; i++) {
		pid = rfork_ids(RFPROC | RFFDG, &ids);
		if (pid == 0)
			child_exit(ids.ri_pid == child_getpid() ? 0 : 1);
		if (pid < 0 || waitpid(pid, &status, 0) != pid ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			return ((void *)1);
	}
	return (NULL);
}

ATF_TC(concurrent);
ATF_TC_HEAD(concurrent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Concurrent forks return valid ids");
	atf_tc_set_md_var(tc, "timeout", "60");
}
ATF_TC_BODY(concurrent, tc)
{
	pthread_t threads[4];
	void *result;
	size_t i;

	for (i = 0; i < nitems(threads); i++)
		ATF_REQUIRE_EQ(pthread_create(&threads[i], NULL, fork_worker,
		    NULL), 0);
	for (i = 0; i < nitems(threads); i++) {
		ATF_REQUIRE_EQ(pthread_join(threads[i], &result), 0);
		ATF_REQUIRE(result == NULL);
	}
}

ATF_TC(ordinary);
ATF_TC_HEAD(ordinary, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Existing fork calls keep their results");
}
ATF_TC_BODY(ordinary, tc)
{
	pid_t pid;
	int fd, i;

	for (i = 0; i < 4; i++) {
		fd = -1;
		switch (i) {
		case 0:
			pid = fork();
			break;
		case 1:
			pid = vfork();
			break;
		case 2:
			pid = rfork(RFPROC | RFFDG);
			break;
		default:
			pid = pdfork(&fd, 0);
			break;
		}
		if (pid == 0)
			_exit(0);
		check_child(pid, 0);
		if (fd != -1)
			ATF_REQUIRE_EQ(close(fd), 0);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ids);
	ATF_TP_ADD_TC(tp, flags);
	ATF_TP_ADD_TC(tp, address);
	ATF_TP_ADD_TC(tp, child_fault);
	ATF_TP_ADD_TC(tp, signal);
	ATF_TP_ADD_TC(tp, capsicum);
	ATF_TP_ADD_TC(tp, shared);
	ATF_TP_ADD_TC(tp, limit);
	ATF_TP_ADD_TC(tp, memory);
	ATF_TP_ADD_TC(tp, concurrent);
	ATF_TP_ADD_TC(tp, ordinary);
	return (atf_no_error());
}
