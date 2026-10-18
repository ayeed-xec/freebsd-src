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
#include <sys/uio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static void
write_file(const char *path, const char *data)
{
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", path, strerror(errno));
	ATF_REQUIRE_EQ((ssize_t)strlen(data), write(fd, data, strlen(data)));
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(syncfs_file);
ATF_TC_BODY(syncfs_file, tc)
{
	int fd;

	write_file("file", "data");

	fd = open("file", O_RDONLY);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_EQ_MSG(0, syncfs(fd), "read-only fd: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, close(fd));

	fd = open("file", O_RDWR);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_EQ_MSG(0, syncfs(fd), "read-write fd: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, close(fd));

	fd = open(".", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_EQ_MSG(0, syncfs(fd), "directory: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(syncfs_data);
ATF_TC_BODY(syncfs_data, tc)
{
	char buf[16];
	char *p;
	long pgsz;
	int fd;

	pgsz = sysconf(_SC_PAGESIZE);
	fd = open("file", O_RDWR | O_CREAT | O_TRUNC, 0644);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, ftruncate(fd, pgsz));
	p = mmap(NULL, pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	memcpy(p, "mapped", 6);
	ATF_REQUIRE_EQ(4, pwrite(fd, "data", 4, 8));
	ATF_REQUIRE_EQ_MSG(0, syncfs(fd), "syncfs: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, munmap(p, pgsz));
	ATF_REQUIRE_EQ(0, close(fd));

	fd = open("file", O_RDONLY);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(12, pread(fd, buf, 12, 0));
	ATF_CHECK(memcmp(buf, "mapped", 6) == 0);
	ATF_CHECK(memcmp(buf + 8, "data", 4) == 0);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(syncfs_ebadf);
ATF_TC_BODY(syncfs_ebadf, tc)
{
	int fd;

	ATF_CHECK_ERRNO(EBADF, syncfs(-1) == -1);

	fd = open(".", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_CHECK_ERRNO(EBADF, syncfs(fd) == -1);

	fd = open(".", O_PATH);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_ERRNO(EBADF, syncfs(fd) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(syncfs_einval);
ATF_TC_BODY(syncfs_einval, tc)
{
	int fd, sv[2];

	ATF_REQUIRE_EQ(0, pipe(sv));
	ATF_CHECK_ERRNO(EINVAL, syncfs(sv[0]) == -1);
	ATF_REQUIRE_EQ(0, close(sv[0]));
	ATF_REQUIRE_EQ(0, close(sv[1]));

	ATF_REQUIRE_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
	ATF_CHECK_ERRNO(EINVAL, syncfs(sv[0]) == -1);
	ATF_REQUIRE_EQ(0, close(sv[0]));
	ATF_REQUIRE_EQ(0, close(sv[1]));

	fd = shm_open(SHM_ANON, O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_ERRNO(EINVAL, syncfs(fd) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(syncfs_capsicum);
ATF_TC_BODY(syncfs_capsicum, tc)
{
	cap_rights_t rights;
	int fd;

	if (!feature_present("security_capabilities"))
		atf_tc_skip("Capsicum is not in the kernel");

	write_file("file", "data");
	fd = open("file", O_RDONLY);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, cap_rights_limit(fd,
	    cap_rights_init(&rights, CAP_READ, CAP_FSYNC)));
	ATF_CHECK_EQ_MSG(0, syncfs(fd), "CAP_FSYNC: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, cap_rights_limit(fd,
	    cap_rights_init(&rights, CAP_READ)));
	ATF_CHECK_ERRNO(ENOTCAPABLE, syncfs(fd) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

static void
mount_nullfs(const char *target, const char *fspath)
{
	struct iovec iov[6];

	iov[0].iov_base = __DECONST(char *, "fstype");
	iov[0].iov_len = sizeof("fstype");
	iov[1].iov_base = __DECONST(char *, "nullfs");
	iov[1].iov_len = sizeof("nullfs");
	iov[2].iov_base = __DECONST(char *, "fspath");
	iov[2].iov_len = sizeof("fspath");
	iov[3].iov_base = __DECONST(char *, fspath);
	iov[3].iov_len = strlen(fspath) + 1;
	iov[4].iov_base = __DECONST(char *, "target");
	iov[4].iov_len = sizeof("target");
	iov[5].iov_base = __DECONST(char *, target);
	iov[5].iov_len = strlen(target) + 1;
	ATF_REQUIRE_EQ_MSG(0, nmount(iov, nitems(iov), 0), "nmount: %s",
	    strerror(errno));
}

ATF_TC_WITH_CLEANUP(syncfs_nullfs);
ATF_TC_HEAD(syncfs_nullfs, tc)
{
	atf_tc_set_md_var(tc, "descr", "syncfs on a file under nullfs");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.kmods", "nullfs");
}
ATF_TC_BODY(syncfs_nullfs, tc)
{
	char lower[PATH_MAX], upper[PATH_MAX];
	int fd;

	ATF_REQUIRE_EQ(0, mkdir("lower", 0755));
	ATF_REQUIRE_EQ(0, mkdir("upper", 0755));
	ATF_REQUIRE(realpath("lower", lower) != NULL);
	ATF_REQUIRE(realpath("upper", upper) != NULL);
	mount_nullfs(lower, upper);
	write_file("upper/file", "data");

	fd = open("upper/file", O_RDONLY);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_EQ_MSG(0, syncfs(fd), "file: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, close(fd));

	fd = open("upper", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_EQ_MSG(0, syncfs(fd), "mount point: %s", strerror(errno));
	ATF_REQUIRE_EQ(0, close(fd));
}
ATF_TC_CLEANUP(syncfs_nullfs, tc)
{
	(void)unmount("upper", MNT_FORCE);
}

ATF_TC_WITHOUT_HEAD(sync_unchanged);
ATF_TC_BODY(sync_unchanged, tc)
{
	/* sync(2) has no result; it must still return. */
	sync();
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, syncfs_file);
	ATF_TP_ADD_TC(tp, syncfs_data);
	ATF_TP_ADD_TC(tp, syncfs_ebadf);
	ATF_TP_ADD_TC(tp, syncfs_einval);
	ATF_TP_ADD_TC(tp, syncfs_capsicum);
	ATF_TP_ADD_TC(tp, syncfs_nullfs);
	ATF_TP_ADD_TC(tp, sync_unchanged);

	return (atf_no_error());
}
