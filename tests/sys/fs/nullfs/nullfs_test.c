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
 * Many processes resolve one path through a nullfs mount at the same time.
 * Each lookup takes a reference on nullfs vnodes that are often idle, with
 * no use and no hold count.  An INVARIANTS kernel on a weakly ordered CPU
 * used to panic in vget_finish_ref() under this load.
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntopts.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	REALPATH_CALLS	100000
#define	REALPATH_FILE	"upper/link/d3/file"
#define	REALPATH_MAXPROC	16

static void __dead2
realpath_loop(const char *expect)
{
	char got[PATH_MAX];
	long i;

	for (i = 0; i < REALPATH_CALLS; i++) {
		if (realpath(REALPATH_FILE, got) == NULL ||
		    strcmp(expect, got) != 0)
			_exit(1);
	}
	_exit(0);
}

static void
mount_nullfs(const char *target, const char *fspath)
{
	struct iovec *iov;
	char errmsg[256];
	int error, iovlen;

	iov = NULL;
	iovlen = 0;
	build_iovec(&iov, &iovlen, "fstype", __DECONST(char *, "nullfs"),
	    (size_t)-1);
	build_iovec(&iov, &iovlen, "fspath", __DECONST(char *, fspath),
	    (size_t)-1);
	build_iovec(&iov, &iovlen, "target", __DECONST(char *, target),
	    (size_t)-1);
	build_iovec(&iov, &iovlen, "errmsg", errmsg, sizeof(errmsg));
	errmsg[0] = '\0';
	error = nmount(iov, iovlen, 0);
	if (error != 0 && errno == ENODEV)
		atf_tc_skip("nullfs is not available");
	ATF_REQUIRE_MSG(error == 0, "nmount: %s",
	    errmsg[0] != '\0' ? errmsg : strerror(errno));
	free_iovec(&iov, &iovlen);
}

ATF_TC_WITH_CLEANUP(parallel_realpath);
ATF_TC_HEAD(parallel_realpath, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "realpath(3) of one nullfs path from many processes at once");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "300");
}
ATF_TC_BODY(parallel_realpath, tc)
{
	char expect[PATH_MAX], got[PATH_MAX];
	struct statfs sfs;
	pid_t pid;
	long ncpu;
	int fd, nproc, p, status;

	ATF_REQUIRE_EQ(0, mkdir("lower", 0755));
	ATF_REQUIRE_EQ(0, mkdir("lower/d1", 0755));
	ATF_REQUIRE_EQ(0, mkdir("lower/d1/d2", 0755));
	ATF_REQUIRE_EQ(0, mkdir("lower/d1/d2/d3", 0755));
	fd = open("lower/d1/d2/d3/file", O_WRONLY | O_CREAT, 0644);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_REQUIRE_EQ(0, symlink("d1/d2", "lower/link"));
	ATF_REQUIRE_EQ(0, mkdir("upper", 0755));
	mount_nullfs("lower", "upper");
	ATF_REQUIRE_EQ(0, statfs("upper", &sfs));
	ATF_REQUIRE_STREQ("nullfs", sfs.f_fstypename);

	/* The symbolic link makes each lookup visit a VLNK vnode too. */
	ATF_REQUIRE(realpath("upper", expect) != NULL);
	ATF_REQUIRE(strlcat(expect, "/d1/d2/d3/file", sizeof(expect)) <
	    sizeof(expect));
	ATF_REQUIRE(realpath(REALPATH_FILE, got) != NULL);
	ATF_REQUIRE_STREQ(expect, got);

	ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	nproc = MIN(MAX(ncpu, 2), REALPATH_MAXPROC);
	for (p = 0; p < nproc; p++) {
		pid = fork();
		ATF_REQUIRE(pid != -1);
		if (pid == 0)
			realpath_loop(expect);
	}
	for (p = 0; p < nproc; p++) {
		pid = wait(&status);
		ATF_REQUIRE(pid != -1);
		ATF_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "process %d: status %#x", pid, status);
	}
	ATF_REQUIRE_EQ(0, unmount("upper", 0));
}
ATF_TC_CLEANUP(parallel_realpath, tc)
{
	(void)unmount("upper", MNT_FORCE);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, parallel_realpath);

	return (atf_no_error());
}
