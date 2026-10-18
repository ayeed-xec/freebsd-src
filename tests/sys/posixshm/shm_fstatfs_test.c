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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static void
check_shm(int fd, size_t bsize)
{
	struct statfs sf;
	struct stat sb;

	ATF_REQUIRE_EQ_MSG(0, fstatfs(fd, &sf), "fstatfs: %s",
	    strerror(errno));
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_CHECK_EQ(STATFS_VERSION, sf.f_version);
	ATF_CHECK_STREQ("shm", sf.f_fstypename);
	ATF_CHECK_STREQ("shm", sf.f_mntfromname);
	ATF_CHECK_STREQ("", sf.f_mntonname);
	ATF_CHECK_EQ(MNT_LOCAL, sf.f_flags);
	ATF_CHECK_EQ(bsize, sf.f_bsize);
	ATF_CHECK_EQ((uint64_t)getpagesize(), sf.f_iosize);
	ATF_CHECK_EQ(0, sf.f_blocks);
	ATF_CHECK_EQ(0, sf.f_bfree);
	ATF_CHECK_EQ(0, sf.f_bavail);
	ATF_CHECK_EQ(0, sf.f_files);
	ATF_CHECK_EQ(0, sf.f_ffree);
	ATF_CHECK_EQ(NAME_MAX, sf.f_namemax);
	ATF_CHECK_EQ((int32_t)sb.st_dev, sf.f_fsid.val[0]);
}

ATF_TC_WITHOUT_HEAD(memfd);
ATF_TC_BODY(memfd, tc)
{
	int fd;

	fd = memfd_create("shm_fstatfs_test", MFD_CLOEXEC);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, ftruncate(fd, 3 * getpagesize()));
	check_shm(fd, getpagesize());
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(anon);
ATF_TC_BODY(anon, tc)
{
	int fd;

	fd = shm_open(SHM_ANON, O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	check_shm(fd, getpagesize());
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(named);
ATF_TC_BODY(named, tc)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/shm_fstatfs_test.%d", (int)getpid());
	fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "shm_open: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, shm_unlink(path));
	check_shm(fd, getpagesize());
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(largepage);
ATF_TC_BODY(largepage, tc)
{
	size_t ps[MAXPAGESIZES];
	int fd, n;

	n = getpagesizes(ps, nitems(ps));
	ATF_REQUIRE(n > 0);
	if (n < 2)
		atf_tc_skip("no large pages");
	fd = shm_create_largepage(SHM_ANON, O_RDWR, 1,
	    SHM_LARGEPAGE_ALLOC_DEFAULT, 0600);
	if (fd < 0 && errno == ENOTTY)
		atf_tc_skip("large page shared memory is not supported");
	ATF_REQUIRE_MSG(fd >= 0, "shm_create_largepage: %s", strerror(errno));
	check_shm(fd, ps[1]);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(capsicum);
ATF_TC_BODY(capsicum, tc)
{
	struct statfs sf;
	cap_rights_t rights;
	int fd;

	if (!feature_present("security_capabilities"))
		atf_tc_skip("Capsicum is not in the kernel");
	fd = shm_open(SHM_ANON, O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, cap_rights_limit(fd,
	    cap_rights_init(&rights, CAP_READ, CAP_FSTATFS)));
	ATF_CHECK_EQ(0, fstatfs(fd, &sf));
	ATF_REQUIRE_EQ(0, cap_rights_limit(fd,
	    cap_rights_init(&rights, CAP_READ)));
	ATF_CHECK_ERRNO(ENOTCAPABLE, fstatfs(fd, &sf) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

/* The other descriptor types keep their old result. */
ATF_TC_WITHOUT_HEAD(unchanged);
ATF_TC_BODY(unchanged, tc)
{
	struct statfs sf, sp;
	int fd, sv[2];

	ATF_REQUIRE_EQ(0, pipe(sv));
	ATF_CHECK_ERRNO(EINVAL, fstatfs(sv[0], &sf) == -1);
	ATF_REQUIRE_EQ(0, close(sv[0]));
	ATF_REQUIRE_EQ(0, close(sv[1]));

	ATF_REQUIRE_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	ATF_CHECK_ERRNO(EINVAL, fstatfs(sv[0], &sf) == -1);
	ATF_REQUIRE_EQ(0, close(sv[0]));
	ATF_REQUIRE_EQ(0, close(sv[1]));

	fd = open("file", O_RDWR | O_CREAT, 0644);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, fstatfs(fd, &sf));
	ATF_REQUIRE_EQ(0, statfs("file", &sp));
	ATF_CHECK_STREQ(sp.f_fstypename, sf.f_fstypename);
	ATF_CHECK_STREQ(sp.f_mntonname, sf.f_mntonname);
	ATF_CHECK_EQ(sp.f_fsid.val[0], sf.f_fsid.val[0]);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, memfd);
	ATF_TP_ADD_TC(tp, anon);
	ATF_TP_ADD_TC(tp, named);
	ATF_TP_ADD_TC(tp, largepage);
	ATF_TP_ADD_TC(tp, capsicum);
	ATF_TP_ADD_TC(tp, unchanged);

	return (atf_no_error());
}
