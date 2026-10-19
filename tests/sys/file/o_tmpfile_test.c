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
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static void setup(const atf_tc_t *);
static int newfile(int, mode_t);
static void check_io(int);
static void *worker(void *);

static void
setup(const atf_tc_t *tc)
{
	char path[] = "unnamed.XXXXXX";

	ATF_REQUIRE_EQ(0,
	    chdir(atf_tc_get_config_var_wd(tc, "directory", ".")));
	ATF_REQUIRE(mkdtemp(path) != NULL);
	ATF_REQUIRE_EQ(0, chdir(path));
}

static int
newfile(int flags, mode_t mode)
{
	int fd;

	fd = open(".", O_TMPFILE | flags, mode);
	ATF_REQUIRE_MSG(fd >= 0, "open: %s", strerror(errno));
	return (fd);
}

static void
check_io(int fd)
{
	char buf[5];

	ATF_REQUIRE_EQ(5, pwrite(fd, "hello", 5, 0));
	ATF_REQUIRE_EQ(5, pread(fd, buf, sizeof(buf), 0));
	ATF_REQUIRE_EQ(0, memcmp(buf, "hello", sizeof(buf)));
}

ATF_TC_WITHOUT_HEAD(basic);
ATF_TC_BODY(basic, tc)
{
	struct stat before, after, sb;
	struct dirent *de;
	DIR *dir;
	int fd, entries;

	setup(tc);
	ATF_REQUIRE_EQ(0x10000000, O_TMPFILE);
	ATF_REQUIRE_EQ(0, stat(".", &before));
	fd = newfile(O_RDWR, 0600);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE(S_ISREG(sb.st_mode));
	ATF_REQUIRE_EQ(0, sb.st_nlink);
	ATF_REQUIRE_EQ(geteuid(), sb.st_uid);
	ATF_REQUIRE_EQ(before.st_gid, sb.st_gid);
	check_io(fd);
	ATF_REQUIRE_EQ(0, fsync(fd));
	ATF_REQUIRE_EQ(0, stat(".", &after));
	ATF_REQUIRE_EQ(before.st_nlink, after.st_nlink);
	ATF_REQUIRE_EQ(before.st_size, after.st_size);
	ATF_REQUIRE_EQ(before.st_mtim.tv_sec, after.st_mtim.tv_sec);
	ATF_REQUIRE_EQ(before.st_mtim.tv_nsec, after.st_mtim.tv_nsec);
	dir = opendir(".");
	ATF_REQUIRE(dir != NULL);
	entries = 0;
	while ((de = readdir(dir)) != NULL) {
		ATF_REQUIRE(strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0);
		entries++;
	}
	ATF_REQUIRE_EQ(2, entries);
	ATF_REQUIRE_EQ(0, closedir(dir));
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(mode);
ATF_TC_BODY(mode, tc)
{
	struct stat sb;
	int fd;

	setup(tc);
	umask(0027);
	fd = newfile(O_RDWR, 0666);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(0640, sb.st_mode & ALLPERMS);
	ATF_REQUIRE_EQ(0, close(fd));
	fd = openat(AT_FDCWD, ".", O_TMPFILE | O_RDWR, 0666);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(0640, sb.st_mode & ALLPERMS);
	ATF_REQUIRE_EQ(0, close(fd));
	fd = newfile(O_RDWR, 0);
	check_io(fd);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(0, sb.st_mode & ALLPERMS);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(writeonly);
ATF_TC_BODY(writeonly, tc)
{
	char c;
	int fd;

	setup(tc);
	fd = newfile(O_WRONLY, 0600);
	ATF_REQUIRE_EQ(1, write(fd, "x", 1));
	ATF_REQUIRE_ERRNO(EBADF, read(fd, &c, 1) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(invalid);
ATF_TC_BODY(invalid, tc)
{
	const int flags[] = { O_RDONLY, O_ACCMODE, O_RDWR | O_CREAT,
	    O_EXEC, O_RDWR | O_EXEC, O_SEARCH, O_RDWR | O_PATH,
	    O_RDWR | O_NAMEDATTR };
	u_int i;

	setup(tc);
	for (i = 0; i < nitems(flags); i++)
		ATF_REQUIRE_ERRNO(EINVAL,
		    open(".", O_TMPFILE | flags[i], 0600) == -1);
}

ATF_TC_WITHOUT_HEAD(paths);
ATF_TC_BODY(paths, tc)
{
	int fd;

	setup(tc);
	fd = open("regular", O_CREAT | O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_REQUIRE_ERRNO(ENOTDIR,
	    open("regular", O_TMPFILE | O_RDWR, 0600) == -1);
	ATF_REQUIRE_ERRNO(ENOENT,
	    open("absent", O_TMPFILE | O_RDWR, 0600) == -1);
	ATF_REQUIRE_EQ(0, symlink(".", "symlink"));
	fd = open("symlink", O_TMPFILE | O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	check_io(fd);
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_REQUIRE_ERRNO(EMLINK,
	    open("symlink", O_TMPFILE | O_RDWR | O_NOFOLLOW, 0600) == -1);
	fd = newfile(O_RDWR | O_DIRECTORY | O_TRUNC, 0600);
	check_io(fd);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(empty_path);
ATF_TC_BODY(empty_path, tc)
{
	int dfd, fd;

	setup(tc);
	dfd = open(".", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(dfd >= 0);
	fd = openat(dfd, "", O_TMPFILE | O_RDWR | O_EMPTY_PATH, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "openat: %s", strerror(errno));
	check_io(fd);
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_REQUIRE_EQ(0, close(dfd));
}

ATF_TC_WITHOUT_HEAD(removed_directory);
ATF_TC_BODY(removed_directory, tc)
{
	int dfd;

	setup(tc);
	ATF_REQUIRE_EQ(0, mkdir("removed", 0700));
	dfd = open("removed", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(dfd >= 0);
	ATF_REQUIRE_EQ(0, rmdir("removed"));
	ATF_REQUIRE_ERRNO(ENOENT,
	    openat(dfd, "", O_TMPFILE | O_RDWR | O_EMPTY_PATH, 0600) == -1);
	ATF_REQUIRE_EQ(0, close(dfd));
}

ATF_TC(access);
ATF_TC_HEAD(access, tc)
{
	atf_tc_set_md_var(tc, "require.user", "unprivileged");
}
ATF_TC_BODY(access, tc)
{

	setup(tc);
	ATF_REQUIRE_EQ(0, mkdir("no_write", 0500));
	ATF_REQUIRE_EQ(0, mkdir("no_search", 0600));
	ATF_REQUIRE_ERRNO(EACCES,
	    open("no_write", O_TMPFILE | O_RDWR, 0600) == -1);
	ATF_REQUIRE_ERRNO(EACCES,
	    open("no_search", O_TMPFILE | O_RDWR, 0600) == -1);
	ATF_REQUIRE_EQ(0, chmod("no_write", 0700));
	ATF_REQUIRE_EQ(0, chmod("no_search", 0700));
}

ATF_TC_WITHOUT_HEAD(immutable);
ATF_TC_BODY(immutable, tc)
{
	int error, fd;

	setup(tc);
	ATF_REQUIRE_EQ(0, mkdir("immutable", 0700));
	ATF_REQUIRE_EQ(0, chflags("immutable", UF_IMMUTABLE));
	fd = open("immutable", O_TMPFILE | O_RDWR, 0600);
	error = errno;
	ATF_REQUIRE_EQ(0, chflags("immutable", 0));
	ATF_REQUIRE_EQ(-1, fd);
	ATF_REQUIRE_EQ(EPERM, error);
}

ATF_TC(unsupported);
ATF_TC_HEAD(unsupported, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(unsupported, tc)
{

	ATF_REQUIRE_ERRNO(EOPNOTSUPP,
	    open("/dev", O_TMPFILE | O_RDWR, 0600) == -1);
}

ATF_TC(fhopen_reject);
ATF_TC_HEAD(fhopen_reject, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(fhopen_reject, tc)
{
	fhandle_t fh;

	setup(tc);
	ATF_REQUIRE_EQ(0, getfh(".", &fh));
	ATF_REQUIRE_ERRNO(EINVAL, fhopen(&fh, O_TMPFILE | O_RDWR) == -1);
}

ATF_TC(link_name);
ATF_TC_HEAD(link_name, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(link_name, tc)
{
	struct stat sb, named;
	int fd;

	setup(tc);
	umask(0);
	fd = newfile(O_RDWR, 0640);
	check_io(fd);
	ATF_REQUIRE_EQ(0, linkat(fd, "", AT_FDCWD, "named", AT_EMPTY_PATH));
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(0, stat("named", &named));
	ATF_REQUIRE_EQ(sb.st_ino, named.st_ino);
	ATF_REQUIRE_EQ(1, sb.st_nlink);
	ATF_REQUIRE_EQ(0640, named.st_mode & ALLPERMS);
	ATF_REQUIRE_EQ(0, linkat(AT_FDCWD, "named", AT_FDCWD, "second", 0));
	ATF_REQUIRE_EQ(0, unlink("named"));
	ATF_REQUIRE_EQ(0, unlink("second"));
	ATF_REQUIRE_ERRNO(ENOENT,
	    linkat(fd, "", AT_FDCWD, "again", AT_EMPTY_PATH) == -1);
	check_io(fd);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC(exclusive);
ATF_TC_HEAD(exclusive, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(exclusive, tc)
{
	int fd;

	setup(tc);
	fd = newfile(O_RDWR | O_EXCL, 0600);
	check_io(fd);
	ATF_REQUIRE_ERRNO(ENOENT,
	    linkat(fd, "", AT_FDCWD, "named", AT_EMPTY_PATH) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC(link_unprivileged);
ATF_TC_HEAD(link_unprivileged, tc)
{
	atf_tc_set_md_var(tc, "require.user", "unprivileged");
}
ATF_TC_BODY(link_unprivileged, tc)
{
	int fd;

	setup(tc);
	fd = newfile(O_RDWR, 0600);
	ATF_REQUIRE_ERRNO(EPERM,
	    linkat(fd, "", AT_FDCWD, "named", AT_EMPTY_PATH) == -1);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(capsicum);
ATF_TC_BODY(capsicum, tc)
{
	cap_rights_t rights;
	int dfd, limited, fd;

	setup(tc);
	dfd = open(".", O_RDONLY | O_DIRECTORY);
	limited = open(".", O_RDONLY | O_DIRECTORY);
	ATF_REQUIRE(dfd >= 0 && limited >= 0);
	cap_rights_init(&rights, CAP_LOOKUP, CAP_READ, CAP_WRITE, CAP_SEEK);
	ATF_REQUIRE_EQ(0, cap_rights_limit(limited, &rights));
	ATF_REQUIRE_ERRNO(ENOTCAPABLE,
	    openat(limited, ".", O_TMPFILE | O_RDWR, 0600) == -1);
	cap_rights_set(&rights, CAP_CREATE);
	ATF_REQUIRE_EQ(0, cap_rights_limit(dfd, &rights));
	ATF_REQUIRE_EQ(0, cap_enter());
	fd = openat(dfd, ".", O_TMPFILE | O_RDWR, 0600);
	ATF_REQUIRE_MSG(fd >= 0, "openat: %s", strerror(errno));
	check_io(fd);
	ATF_REQUIRE_EQ(0, close(fd));
	ATF_REQUIRE_ERRNO(ECAPMODE, open(".", O_TMPFILE | O_RDWR, 0600) == -1);
	ATF_REQUIRE_EQ(0, close(dfd));
	ATF_REQUIRE_EQ(0, close(limited));
}

ATF_TC_WITHOUT_HEAD(ordinary);
ATF_TC_BODY(ordinary, tc)
{
	struct stat sb;
	int fd;

	setup(tc);
	fd = open("named", O_CREAT | O_EXCL | O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	check_io(fd);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(1, sb.st_nlink);
	ATF_REQUIRE_ERRNO(EEXIST,
	    open("named", O_CREAT | O_EXCL | O_RDWR, 0600) == -1);
	ATF_REQUIRE_ERRNO(EISDIR, open(".", O_RDWR) == -1);
	ATF_REQUIRE_EQ(0, linkat(AT_FDCWD, "named", AT_FDCWD, "link", 0));
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(2, sb.st_nlink);
	ATF_REQUIRE_EQ(0, close(fd));
	fd = open("named", O_RDWR | O_TRUNC);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(0, sb.st_size);
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC(ordinary_removed);
ATF_TC_HEAD(ordinary_removed, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(ordinary_removed, tc)
{
	struct statfs fs;
	int fd, link_errno, open_errno, rfd;

	setup(tc);
	ATF_REQUIRE_EQ(0, statfs(".", &fs));
	if (strcmp(fs.f_fstypename, "nullfs") == 0)
		ATF_REQUIRE_EQ(0, statfs(fs.f_mntfromname, &fs));
	/*
	 * A file that was removed while open keeps its old behavior: tmpfs
	 * can give it a new name but does not open it again, UFS does the
	 * opposite.
	 */
	if (strcmp(fs.f_fstypename, "tmpfs") == 0) {
		link_errno = 0;
		open_errno = ENOENT;
	} else if (strcmp(fs.f_fstypename, "ufs") == 0) {
		link_errno = ENOENT;
		open_errno = 0;
	} else {
		atf_tc_skip("no known result for %s", fs.f_fstypename);
	}
	fd = open("removed", O_CREAT | O_EXCL | O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, unlink("removed"));
	rfd = openat(fd, "", O_RDONLY | O_EMPTY_PATH);
	if (open_errno == 0) {
		ATF_REQUIRE_MSG(rfd >= 0, "openat: %s", strerror(errno));
		ATF_REQUIRE_EQ(0, close(rfd));
	} else {
		ATF_REQUIRE_ERRNO(open_errno, rfd == -1);
	}
	if (link_errno == 0) {
		ATF_REQUIRE_EQ(0,
		    linkat(fd, "", AT_FDCWD, "again", AT_EMPTY_PATH));
		ATF_REQUIRE_EQ(0, unlink("again"));
	} else {
		ATF_REQUIRE_ERRNO(link_errno,
		    linkat(fd, "", AT_FDCWD, "again", AT_EMPTY_PATH) == -1);
	}
	ATF_REQUIRE_EQ(0, close(fd));
}

static void *
worker(void *arg __unused)
{
	int fd, i;

	for (i = 0; i < 200; i++) {
		fd = newfile(O_RDWR | O_EXCL, 0600);
		check_io(fd);
		ATF_REQUIRE_EQ(0, close(fd));
	}
	return (NULL);
}

ATF_TC_WITHOUT_HEAD(threads);
ATF_TC_BODY(threads, tc)
{
	pthread_t threads[4];
	u_int i;

	setup(tc);
	for (i = 0; i < nitems(threads); i++)
		ATF_REQUIRE_EQ(0,
		    pthread_create(&threads[i], NULL, worker, NULL));
	for (i = 0; i < nitems(threads); i++)
		ATF_REQUIRE_EQ(0, pthread_join(threads[i], NULL));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, basic);
	ATF_TP_ADD_TC(tp, mode);
	ATF_TP_ADD_TC(tp, writeonly);
	ATF_TP_ADD_TC(tp, invalid);
	ATF_TP_ADD_TC(tp, paths);
	ATF_TP_ADD_TC(tp, empty_path);
	ATF_TP_ADD_TC(tp, removed_directory);
	ATF_TP_ADD_TC(tp, access);
	ATF_TP_ADD_TC(tp, immutable);
	ATF_TP_ADD_TC(tp, unsupported);
	ATF_TP_ADD_TC(tp, fhopen_reject);
	ATF_TP_ADD_TC(tp, link_name);
	ATF_TP_ADD_TC(tp, exclusive);
	ATF_TP_ADD_TC(tp, link_unprivileged);
	ATF_TP_ADD_TC(tp, capsicum);
	ATF_TP_ADD_TC(tp, ordinary);
	ATF_TP_ADD_TC(tp, ordinary_removed);
	ATF_TP_ADD_TC(tp, threads);
	return (atf_no_error());
}
