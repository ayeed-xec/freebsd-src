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
#include <sys/jail.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <mntopts.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static void setup(const atf_tc_t *);
static int newfile(int, mode_t);
static void check_io(int);
static void *worker(void *);
static void check_path(int, const char *);
static void saved_path(int, const char *, char *, size_t);
static void check_filedesc(pid_t, int, const char *);
static void *kinfo_worker(void *);

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

static void
check_path(int fd, const char *want)
{
	struct kinfo_file kif;

	memset(&kif, 0, sizeof(kif));
	kif.kf_structsize = sizeof(kif);
	ATF_REQUIRE_EQ(0, fcntl(fd, F_KINFO, &kif));
	ATF_REQUIRE_STREQ(want, kif.kf_path);
}

static void
saved_path(int fd, const char *dir, char *buf, size_t size)
{
	struct stat sb;
	int n;

	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	n = snprintf(buf, size, "%s/#%ju (deleted)",
	    strcmp(dir, "/") == 0 ? "" : dir, (uintmax_t)sb.st_ino);
	ATF_REQUIRE(n >= 0 && (size_t)n < size);
}

ATF_TC_WITHOUT_HEAD(path_saved);
ATF_TC_BODY(path_saved, tc)
{
	char dir[PATH_MAX], want[PATH_MAX];
	int fd, rfd;

	setup(tc);
	ATF_REQUIRE_EQ(0, mkdir("before", 0700));
	ATF_REQUIRE(realpath("before", dir) != NULL);
	fd = open("before", O_TMPFILE | O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	saved_path(fd, dir, want, sizeof(want));
	check_path(fd, want);
	rfd = openat(fd, "", O_RDONLY | O_EMPTY_PATH);
	ATF_REQUIRE(rfd >= 0);
	check_path(rfd, want);
	ATF_REQUIRE_EQ(0, rename("before", "after"));
	check_path(fd, want);
	ATF_REQUIRE_EQ(0, rmdir("after"));
	check_path(rfd, want);
	ATF_REQUIRE_EQ(0, close(rfd));
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(path_long);
ATF_TC_BODY(path_long, tc)
{
	char name[241];
	int fd, i;

	setup(tc);
	memset(name, 'x', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	for (i = 0; i < 6; i++) {
		ATF_REQUIRE_EQ(0, mkdir(name, 0700));
		ATF_REQUIRE_EQ(0, chdir(name));
	}
	fd = newfile(O_RDWR, 0600);
	check_path(fd, "");
	ATF_REQUIRE_EQ(0, close(fd));
	for (i = 0; i < 6; i++)
		ATF_REQUIRE_EQ(0, chdir(".."));
}

ATF_TC(path_link);
ATF_TC_HEAD(path_link, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(path_link, tc)
{
	struct statfs fs;
	char cwd[PATH_MAX], want[PATH_MAX];
	int fd;

	setup(tc);
	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	ATF_REQUIRE_EQ(0, statfs(".", &fs));
	if (strcmp(fs.f_fstypename, "nullfs") == 0)
		ATF_REQUIRE_EQ(0, statfs(fs.f_mntfromname, &fs));
	fd = newfile(O_RDWR, 0600);
	saved_path(fd, cwd, want, sizeof(want));
	check_path(fd, want);
	ATF_REQUIRE_EQ(0,
	    linkat(fd, "", AT_FDCWD, "named", AT_EMPTY_PATH));
	if (strcmp(fs.f_fstypename, "tmpfs") == 0) {
		ATF_REQUIRE(snprintf(want, sizeof(want), "%s/named", cwd) <
		    (int)sizeof(want));
		check_path(fd, want);
	} else {
		ATF_REQUIRE_STREQ("ufs", fs.f_fstypename);
		check_path(fd, "");
	}
	ATF_REQUIRE_EQ(0, unlink("named"));
	check_path(fd, "");
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITHOUT_HEAD(path_suffix);
ATF_TC_BODY(path_suffix, tc)
{
	char cwd[PATH_MAX], name[241];
	size_t len, n;
	int fd;

	setup(tc);
	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	len = strlen(cwd);
	ATF_REQUIRE(len < 1017);
	while (len < 1018) {
		n = MIN(sizeof(name) - 1, 1018 - len - 1);
		if (len + n + 1 == 1017)
			n--;
		memset(name, 'x', n);
		name[n] = '\0';
		ATF_REQUIRE_EQ(0, mkdir(name, 0700));
		ATF_REQUIRE_EQ(0, chdir(name));
		len += n + 1;
	}
	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	ATF_REQUIRE_EQ(1018, strlen(cwd));
	fd = newfile(O_RDWR, 0600);
	check_path(fd, "");
	ATF_REQUIRE_EQ(0, close(fd));
}

ATF_TC_WITH_CLEANUP(file_mount);
ATF_TC_HEAD(file_mount, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.kmods", "nullfs");
}
ATF_TC_BODY(file_mount, tc)
{
	struct statfs fs;
	char cwd[PATH_MAX], source[PATH_MAX], target[PATH_MAX];
	char errmsg[256];
	struct stat sb, linked, source_sb;
	struct iovec *iov;
	const char *want;
	int fd, iovlen, sourcefd;

	ATF_REQUIRE_EQ(0,
	    chdir(atf_tc_get_config_var_wd(tc, "directory", ".")));
	ATF_REQUIRE_EQ(0, mkdir("file-mount", 0700));
	ATF_REQUIRE_EQ(0, chdir("file-mount"));
	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	ATF_REQUIRE_EQ(0, statfs(".", &fs));
	ATF_REQUIRE(snprintf(source, sizeof(source), "%s/source", cwd) <
	    (int)sizeof(source));
	ATF_REQUIRE(snprintf(target, sizeof(target), "%s/covered", cwd) <
	    (int)sizeof(target));
	sourcefd = open(source, O_CREAT | O_EXCL | O_RDWR, 0600);
	fd = open(target, O_CREAT | O_EXCL | O_RDWR, 0600);
	ATF_REQUIRE(sourcefd >= 0 && fd >= 0);
	ATF_REQUIRE_EQ(0, fstat(sourcefd, &source_sb));
	ATF_REQUIRE_EQ(0, close(sourcefd));
	iov = NULL;
	iovlen = 0;
	build_iovec(&iov, &iovlen, __DECONST(char *, "fstype"),
	    __DECONST(char *, "nullfs"), (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "fspath"),
	    target, (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "target"),
	    source, (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "errmsg"), errmsg,
	    sizeof(errmsg));
	errmsg[0] = '\0';
	ATF_REQUIRE_MSG(nmount(iov, iovlen, 0) == 0, "nmount: %s",
	    errmsg[0] != '\0' ? errmsg : strerror(errno));
	free_iovec(&iov, &iovlen);
	if (strcmp(fs.f_fstypename, "tmpfs") == 0)
		want = target;
	else {
		ATF_REQUIRE_STREQ("ufs", fs.f_fstypename);
		want = "";
	}
	check_path(fd, want);
	check_filedesc(getpid(), fd, want);
	ATF_REQUIRE_EQ(0, linkat(fd, "", AT_FDCWD, "linked", AT_EMPTY_PATH));
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	ATF_REQUIRE_EQ(0, stat("linked", &linked));
	ATF_REQUIRE_EQ(source_sb.st_ino, linked.st_ino);
	ATF_REQUIRE_EQ(1, linked.st_nlink);
	ATF_REQUIRE_EQ(2, sb.st_nlink);
	ATF_REQUIRE_EQ(0, unmount(target, 0));
	ATF_REQUIRE_EQ(0, stat(target, &linked));
	ATF_REQUIRE_EQ(sb.st_ino, linked.st_ino);
	ATF_REQUIRE_EQ(0, stat("linked", &linked));
	ATF_REQUIRE_EQ(sb.st_ino, linked.st_ino);
	ATF_REQUIRE_EQ(0, close(fd));
}
ATF_TC_CLEANUP(file_mount, tc)
{

	if (chdir(atf_tc_get_config_var_wd(tc, "directory", ".")) == 0)
		(void)unmount("file-mount/covered", MNT_FORCE);
}

ATF_TC_WITHOUT_HEAD(path_ordinary);
ATF_TC_BODY(path_ordinary, tc)
{
	struct statfs fs;
	char cwd[PATH_MAX], want[PATH_MAX];
	int fd;

	setup(tc);
	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	ATF_REQUIRE_EQ(0, statfs(".", &fs));
	if (strcmp(fs.f_fstypename, "nullfs") == 0)
		ATF_REQUIRE_EQ(0, statfs(fs.f_mntfromname, &fs));
	fd = open("ordinary", O_CREAT | O_EXCL | O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	if (strcmp(fs.f_fstypename, "tmpfs") == 0) {
		ATF_REQUIRE(snprintf(want, sizeof(want), "%s/ordinary", cwd) <
		    (int)sizeof(want));
		check_path(fd, want);
	} else {
		ATF_REQUIRE_STREQ("ufs", fs.f_fstypename);
		check_path(fd, "");
	}
	ATF_REQUIRE_EQ(0, unlink("ordinary"));
	check_path(fd, "");
	ATF_REQUIRE_EQ(0, close(fd));
}

static void
check_filedesc(pid_t pid, int fd, const char *want)
{
	struct kinfo_file *kif;
	char *buf, *end, *p;
	size_t size;
	int mib[4];
	bool found;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_FILEDESC;
	mib[3] = pid;
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), NULL, &size, NULL, 0));
	size += sizeof(*kif) * 4;
	buf = malloc(size);
	ATF_REQUIRE(buf != NULL);
	ATF_REQUIRE_EQ(0, sysctl(mib, nitems(mib), buf, &size, NULL, 0));
	found = false;
	end = buf + size;
	for (p = buf; p < end; p += kif->kf_structsize) {
		kif = (struct kinfo_file *)p;
		ATF_REQUIRE(kif->kf_structsize > 0);
		ATF_REQUIRE(p + kif->kf_structsize <= end);
		if (kif->kf_fd == fd) {
			ATF_CHECK_STREQ(want, kif->kf_path);
			found = true;
			break;
		}
	}
	free(buf);
	ATF_REQUIRE(found);
}

ATF_TC(path_jail);
ATF_TC_HEAD(path_jail, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(path_jail, tc)
{
	char dir[PATH_MAX], rootwant[PATH_MAX], subwant[PATH_MAX];
	char want[PATH_MAX];
	int ready[2], release[2];
	pid_t child;
	int fd, jid, outside, rootfd, status;
	char byte;

	setup(tc);
	ATF_REQUIRE_EQ(0, mkdir("jail", 0755));
	ATF_REQUIRE_EQ(0, mkdir("jail/sub", 0755));
	ATF_REQUIRE_EQ(0, mkdir("jail-other", 0755));
	outside = open("jail-other", O_TMPFILE | O_RDWR, 0600);
	rootfd = open("jail", O_TMPFILE | O_RDWR, 0600);
	fd = open("jail/sub", O_TMPFILE | O_RDWR, 0600);
	ATF_REQUIRE(outside >= 0 && rootfd >= 0 && fd >= 0);
	saved_path(rootfd, "/", rootwant, sizeof(rootwant));
	saved_path(fd, "/sub", subwant, sizeof(subwant));
	ATF_REQUIRE(realpath("jail/sub", want) != NULL);
	saved_path(fd, want, dir, sizeof(dir));
	ATF_REQUIRE(realpath("jail", want) != NULL);
	ATF_REQUIRE_EQ(0, pipe(ready));
	ATF_REQUIRE_EQ(0, pipe(release));
	child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		ATF_REQUIRE_EQ(0, close(ready[0]));
		ATF_REQUIRE_EQ(0, close(release[1]));
		jid = jail_setv(JAIL_CREATE | JAIL_ATTACH, "path", want, NULL);
		ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
		check_path(rootfd, rootwant);
		check_path(fd, subwant);
		check_path(outside, "");
		ATF_REQUIRE_EQ(1, write(ready[1], "r", 1));
		ATF_REQUIRE_EQ(1, read(release[0], &byte, 1));
		_exit(0);
	}
	ATF_REQUIRE_EQ(0, close(ready[1]));
	ATF_REQUIRE_EQ(0, close(release[0]));
	ATF_REQUIRE_EQ(1, read(ready[0], &byte, 1));
	check_filedesc(child, fd, dir);
	ATF_REQUIRE_EQ(1, write(release[1], "r", 1));
	ATF_REQUIRE_EQ(child, waitpid(child, &status, 0));
	ATF_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ATF_REQUIRE_EQ(0, close(outside));
	ATF_REQUIRE_EQ(0, close(rootfd));
	ATF_REQUIRE_EQ(0, close(fd));
}

struct kinfo_race {
	atomic_uint reads;
	atomic_bool stop;
	int fd;
};

static void *
kinfo_worker(void *arg)
{
	struct kinfo_file kif;
	struct kinfo_race *race;

	race = arg;
	while (!atomic_load(&race->stop)) {
		memset(&kif, 0, sizeof(kif));
		kif.kf_structsize = sizeof(kif);
		(void)fcntl(race->fd, F_KINFO, &kif);
		atomic_fetch_add(&race->reads, 1);
	}
	return (NULL);
}

ATF_TC(forced_unmount);
ATF_TC_HEAD(forced_unmount, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "60");
}
ATF_TC_BODY(forced_unmount, tc)
{
	struct kinfo_race race;
	pthread_t thread;
	const char *dir;
	unsigned int before;

	if (!atf_tc_has_config_var(tc, "unmount"))
		atf_tc_skip("the shell case supplies a private mount");
	dir = atf_tc_get_config_var(tc, "unmount");
	race.fd = open(dir, O_TMPFILE | O_RDWR, 0600);
	ATF_REQUIRE(race.fd >= 0);
	atomic_init(&race.reads, 0);
	atomic_init(&race.stop, false);
	ATF_REQUIRE_EQ(0, pthread_create(&thread, NULL, kinfo_worker, &race));
	while (atomic_load(&race.reads) < 1000)
		sched_yield();
	ATF_REQUIRE_EQ(0, unmount(dir, MNT_FORCE));
	before = atomic_load(&race.reads);
	while (atomic_load(&race.reads) - before < 1000)
		sched_yield();
	atomic_store(&race.stop, true);
	ATF_REQUIRE_EQ(0, pthread_join(thread, NULL));
	ATF_REQUIRE_EQ(0, close(race.fd));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, file_mount);
	ATF_TP_ADD_TC(tp, path_suffix);
	ATF_TP_ADD_TC(tp, path_saved);
	ATF_TP_ADD_TC(tp, path_long);
	ATF_TP_ADD_TC(tp, path_link);
	ATF_TP_ADD_TC(tp, path_ordinary);
	ATF_TP_ADD_TC(tp, path_jail);
	ATF_TP_ADD_TC(tp, forced_unmount);
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
