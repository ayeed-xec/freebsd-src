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
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static long
self_id(void)
{
	long id;

	ATF_REQUIRE_EQ(0, thr_self(&id));
	return (id);
}

ATF_TC_WITHOUT_HEAD(get_set);
ATF_TC_BODY(get_set, tc)
{
	char name[MAXCOMLEN + 1];
	long id;

	id = self_id();
	ATF_REQUIRE_EQ(0, thr_set_name(id, "thr_name_test"));
	ATF_REQUIRE_EQ_MSG(0, thr_get_name(id, name, sizeof(name)),
	    "thr_get_name: %s", strerror(errno));
	ATF_CHECK_STREQ("thr_name_test", name);
}

ATF_TC_WITHOUT_HEAD(truncated);
ATF_TC_BODY(truncated, tc)
{
	char longname[MAXCOMLEN * 2], name[sizeof(longname)];
	long id;

	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	id = self_id();
	ATF_REQUIRE_EQ(0, thr_set_name(id, longname));
	ATF_REQUIRE_EQ(0, thr_get_name(id, name, sizeof(name)));
	ATF_CHECK_EQ(MAXCOMLEN, strlen(name));
	ATF_CHECK(strncmp(longname, name, MAXCOMLEN) == 0);
}

static pthread_barrier_t barrier;
static long other_id;

static void *
other_main(void *arg __unused)
{
	ATF_REQUIRE_EQ(0, thr_self(&other_id));
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	return (NULL);
}

ATF_TC_WITHOUT_HEAD(other_thread);
ATF_TC_BODY(other_thread, tc)
{
	char comm[MAXCOMLEN + 1], name[MAXCOMLEN + 1];
	struct kinfo_proc *kp;
	pthread_t thr;
	size_t i, len;
	int mib[4];
	bool found;

	ATF_REQUIRE_EQ(0, pthread_barrier_init(&barrier, NULL, 2));
	ATF_REQUIRE_EQ(0, pthread_create(&thr, NULL, other_main, NULL));
	pthread_barrier_wait(&barrier);

	/* libthr names no thread, so the new thread has the process name. */
	strlcpy(comm, getprogname(), sizeof(comm));
	ATF_REQUIRE_EQ(0, thr_get_name(other_id, name, sizeof(name)));
	ATF_CHECK_STREQ(comm, name);

	ATF_REQUIRE_EQ(0, thr_set_name(other_id, "other"));
	ATF_REQUIRE_EQ(0, thr_get_name(other_id, name, sizeof(name)));
	ATF_CHECK_STREQ("other", name);

	/* The value of kern.proc.pid is the same. */
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID | KERN_PROC_INC_THREAD;
	mib[3] = getpid();
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), NULL, &len, NULL, 0));
	kp = malloc(len);
	ATF_REQUIRE(kp != NULL);
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), kp, &len, NULL, 0));
	found = false;
	for (i = 0; i < len / sizeof(*kp); i++) {
		if (kp[i].ki_tid == other_id) {
			ATF_CHECK_STREQ(kp[i].ki_tdname, name);
			found = true;
		}
	}
	ATF_CHECK(found);
	free(kp);

	pthread_barrier_wait(&barrier);
	ATF_REQUIRE_EQ(0, pthread_join(thr, NULL));
}

ATF_TC_WITHOUT_HEAD(erange);
ATF_TC_BODY(erange, tc)
{
	char name[MAXCOMLEN + 1];
	long id;

	id = self_id();
	ATF_REQUIRE_EQ(0, thr_set_name(id, "abcd"));
	ATF_CHECK_ERRNO(ERANGE, thr_get_name(id, name, 4) == -1);
	ATF_CHECK_ERRNO(ERANGE, thr_get_name(id, name, 0) == -1);
	ATF_REQUIRE_EQ(0, thr_get_name(id, name, 5));
	ATF_CHECK_STREQ("abcd", name);
}

ATF_TC_WITHOUT_HEAD(esrch);
ATF_TC_BODY(esrch, tc)
{
	char c, name[MAXCOMLEN + 1];
	long id;
	pid_t pid;
	int status, tochild[2], toparent[2];

	ATF_CHECK_ERRNO(ESRCH, thr_get_name(0, name, sizeof(name)) == -1);
	ATF_CHECK_ERRNO(ESRCH, thr_get_name(-1, name, sizeof(name)) == -1);

	/* A thread of another live process is not visible. */
	ATF_REQUIRE_EQ(0, pipe(tochild));
	ATF_REQUIRE_EQ(0, pipe(toparent));
	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		if (thr_self(&id) != 0 ||
		    write(toparent[1], &id, sizeof(id)) != sizeof(id) ||
		    read(tochild[0], &c, 1) != 1)
			_exit(1);
		_exit(0);
	}
	ATF_REQUIRE_EQ(sizeof(id), read(toparent[0], &id, sizeof(id)));
	ATF_CHECK_ERRNO(ESRCH, thr_get_name(id, name, sizeof(name)) == -1);
	ATF_REQUIRE_EQ(1, write(tochild[1], "x", 1));
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

ATF_TC_WITHOUT_HEAD(efault);
ATF_TC_BODY(efault, tc)
{
	char *p;
	long pgsz;

	pgsz = sysconf(_SC_PAGESIZE);
	p = mmap(NULL, pgsz, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_CHECK_ERRNO(EFAULT, thr_get_name(self_id(), p, pgsz) == -1);
	ATF_REQUIRE_EQ(0, munmap(p, pgsz));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, get_set);
	ATF_TP_ADD_TC(tp, truncated);
	ATF_TP_ADD_TC(tp, other_thread);
	ATF_TP_ADD_TC(tp, erange);
	ATF_TP_ADD_TC(tp, esrch);
	ATF_TP_ADD_TC(tp, efault);

	return (atf_no_error());
}
