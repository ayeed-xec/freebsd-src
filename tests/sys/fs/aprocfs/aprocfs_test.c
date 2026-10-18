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
 * Tests of aprocfs(5).  Each node is parsed with the rules of the Android
 * code that reads it: libprocinfo for stat, status and maps.
 */

#include <sys/param.h>
#include <sys/jail.h>
#include <sys/linker.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sched.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/thr.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <mntopts.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	MNT	"mnt"

static void
mount_aprocfs(void)
{
	struct iovec *iov;
	char errmsg[256];
	int error, iovlen;

	ATF_REQUIRE_EQ(0, mkdir(MNT, 0755));
	iov = NULL;
	iovlen = 0;
	build_iovec(&iov, &iovlen, __DECONST(char *, "fstype"),
	    __DECONST(char *, "aprocfs"), (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "fspath"),
	    __DECONST(char *, MNT), (size_t)-1);
	build_iovec(&iov, &iovlen, __DECONST(char *, "errmsg"), errmsg,
	    sizeof(errmsg));
	errmsg[0] = '\0';
	error = nmount(iov, iovlen, 0);
	ATF_REQUIRE_MSG(error == 0, "nmount: %s",
	    errmsg[0] != '\0' ? errmsg : strerror(errno));
	free_iovec(&iov, &iovlen);
}

/* Read a whole node; return its length.  The buffer ends with a NUL. */
static ssize_t
read_node(const char *path, char *buf, size_t size)
{
	ssize_t len, n;
	int fd;

	fd = open(path, O_RDONLY);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", path, strerror(errno));
	len = 0;
	while ((n = read(fd, buf + len, size - 1 - len)) > 0)
		len += n;
	ATF_REQUIRE_MSG(n == 0, "read %s: %s", path, strerror(errno));
	ATF_REQUIRE((size_t)len < size - 1);
	buf[len] = '\0';
	close(fd);
	return (len);
}

static long
self_tid(void)
{
	long tid;

	ATF_REQUIRE_EQ(0, thr_self(&tid));
	return (tid);
}

static bool
dir_has(const char *path, const char *name)
{
	struct dirent *de;
	DIR *d;
	bool found;

	d = opendir(path);
	ATF_REQUIRE_MSG(d != NULL, "opendir %s: %s", path, strerror(errno));
	found = false;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, name) == 0)
			found = true;
	}
	closedir(d);
	return (found);
}

/* The fields of stat after the comm, as libprocinfo splits them. */
static int
stat_fields(const char *stat, char fields[][32], int max)
{
	const char *p;
	size_t len;
	int n;

	p = strrchr(stat, ')');
	ATF_REQUIRE(p != NULL && p[1] == ' ');
	p += 2;
	for (n = 0; n < max && *p != '\0' && *p != '\n'; n++) {
		len = strcspn(p, " \n");
		ATF_REQUIRE(len < 32);
		memcpy(fields[n], p, len);
		fields[n][len] = '\0';
		p += len;
		if (*p == ' ')
			p++;
	}
	return (n);
}

/* The value of a status line, as libprocinfo finds it. */
static bool
status_field(const char *status, const char *header, char *val, size_t size)
{
	const char *line, *tab, *nl;
	size_t len;

	for (line = status; *line != '\0'; line = nl + 1) {
		nl = strchr(line, '\n');
		ATF_REQUIRE(nl != NULL);
		tab = strchr(line, '\t');
		if (tab == NULL || tab > nl)
			continue;
		if ((size_t)(tab - line) == strlen(header) &&
		    strncmp(line, header, tab - line) == 0) {
			len = nl - tab - 1;
			ATF_REQUIRE(len < size);
			memcpy(val, tab + 1, len);
			val[len] = '\0';
			return (true);
		}
	}
	return (false);
}

/* One maps line, with the checks of libprocinfo ParseMapsFileLine(). */
struct mapline {
	unsigned long	start;
	unsigned long	end;
	char		perms[5];
	unsigned long	pgoff;
	unsigned long	inode;
	const char	*name;
};

static void
parse_mapline(char *line, struct mapline *ml)
{
	char *p, *end;

	p = line;
	ml->start = strtoul(p, &end, 16);
	ATF_REQUIRE(end != p && *end == '-');
	p = end + 1;
	ml->end = strtoul(p, &end, 16);
	ATF_REQUIRE(end != p && *end == ' ');
	p = end + 1;
	ATF_REQUIRE(strchr("r-", p[0]) != NULL);
	ATF_REQUIRE(strchr("w-", p[1]) != NULL);
	ATF_REQUIRE(strchr("x-", p[2]) != NULL);
	ATF_REQUIRE(strchr("ps", p[3]) != NULL && p[4] == ' ');
	memcpy(ml->perms, p, 4);
	ml->perms[4] = '\0';
	p += 5;
	ml->pgoff = strtoul(p, &end, 16);
	ATF_REQUIRE(end != p && *end == ' ');
	p = end + 1;
	strtoul(p, &end, 16);
	ATF_REQUIRE(end != p && *end == ':');
	p = end + 1;
	strtoul(p, &end, 16);
	ATF_REQUIRE(end != p && *end == ' ');
	p = end + 1;
	ml->inode = strtoul(p, &end, 10);
	ATF_REQUIRE(end != p && (*end == '\0' || *end == ' '));
	p = end;
	while (*p == ' ')
		p++;
	ml->name = p;
}

static void
proc_path(char *buf, size_t size, pid_t pid, const char *node)
{

	snprintf(buf, size, MNT "/%d/%s", (int)pid, node);
}

#define	APROCFS_TC(name, description)					\
	ATF_TC_WITH_CLEANUP(name);					\
	ATF_TC_HEAD(name, tc)						\
	{								\
		atf_tc_set_md_var(tc, "descr", description);		\
		atf_tc_set_md_var(tc, "require.user", "root");		\
		atf_tc_set_md_var(tc, "require.kmods", "aprocfs");	\
		atf_tc_set_md_var(tc, "timeout", "60");			\
	}								\
	ATF_TC_CLEANUP(name, tc)					\
	{								\
		(void)unmount(MNT, MNT_FORCE);				\
	}

APROCFS_TC(root, "The root lists self, sys, version and the processes");
ATF_TC_BODY(root, tc)
{
	char name[32];
	struct stat sb;

	mount_aprocfs();
	ATF_CHECK(dir_has(MNT, "self"));
	ATF_CHECK(dir_has(MNT, "sys"));
	ATF_CHECK(dir_has(MNT, "version"));
	snprintf(name, sizeof(name), "%d", getpid());
	ATF_CHECK(dir_has(MNT, name));
	/* A thread id resolves but is not listed, as on Linux. */
	snprintf(name, sizeof(name), "%ld", self_tid());
	ATF_CHECK(!dir_has(MNT, name));
	snprintf(name, sizeof(name), MNT "/%ld", self_tid());
	ATF_REQUIRE_EQ(0, stat(name, &sb));
	ATF_CHECK(S_ISDIR(sb.st_mode));
	ATF_CHECK_ERRNO(ENOENT, stat(MNT "/0", &sb) == -1);
	ATF_CHECK_ERRNO(ENOENT, stat(MNT "/01", &sb) == -1);
}

APROCFS_TC(self, "self is a link to the pid of the reader");
ATF_TC_BODY(self, tc)
{
	char buf[32], want[32];
	ssize_t len;

	mount_aprocfs();
	len = readlink(MNT "/self", buf, sizeof(buf));
	ATF_REQUIRE(len > 0);
	buf[len] = '\0';
	snprintf(want, sizeof(want), "%d", getpid());
	ATF_CHECK_STREQ(want, buf);
}

APROCFS_TC(version, "version has the FreeBSD uname values");
ATF_TC_BODY(version, tc)
{
	char buf[1024], release[64], want[128];
	size_t len;

	mount_aprocfs();
	read_node(MNT "/version", buf, sizeof(buf));
	len = sizeof(release);
	ATF_REQUIRE_EQ(0, sysctlbyname("kern.osrelease", release, &len, NULL,
	    0));
	snprintf(want, sizeof(want), "FreeBSD version %s (", release);
	ATF_CHECK_MSG(strncmp(buf, want, strlen(want)) == 0, "%s", buf);
	len = strlen(buf);
	ATF_CHECK(len > 2 && strcmp(buf + len - 2, ")\n") == 0);
}

APROCFS_TC(pid_max, "pid_max is one above the largest thread id");
ATF_TC_BODY(pid_max, tc)
{
	char buf[64];
	size_t len;
	long pidmax;
	int maxthread;

	mount_aprocfs();
	read_node(MNT "/sys/kernel/pid_max", buf, sizeof(buf));
	ATF_REQUIRE_EQ(1, sscanf(buf, "%ld", &pidmax));
	len = sizeof(maxthread);
	ATF_REQUIRE_EQ(0, sysctlbyname("kern.maxthread", &maxthread, &len,
	    NULL, 0));
	ATF_CHECK_EQ(100000 + maxthread, pidmax);
	ATF_CHECK(pidmax > self_tid());
	ATF_CHECK(pidmax > getpid());
}

APROCFS_TC(stat, "stat has 52 fields in the Linux order");
ATF_TC_BODY(stat, tc)
{
	char buf[2048], comm[32], fields[60][32];
	const char *p;
	int n, nice;

	mount_aprocfs();
	read_node(MNT "/self/stat", buf, sizeof(buf));
	ATF_CHECK_EQ(getpid(), atoi(buf));
	p = strchr(buf, '(');
	ATF_REQUIRE(p != NULL);
	strlcpy(comm, getprogname(), 16);
	ATF_CHECK_MSG(strncmp(p + 1, comm, strlen(comm)) == 0 &&
	    p[1 + strlen(comm)] == ')', "%s", buf);
	n = stat_fields(buf, fields, 60);
	/* pid and comm are fields 1 and 2; fields[0] is field 3. */
	ATF_REQUIRE_EQ(50, n);
	ATF_CHECK_STREQ("R", fields[0]);
	ATF_CHECK_EQ(getppid(), atoi(fields[1]));
	ATF_CHECK_EQ(getpgrp(), atoi(fields[2]));
	ATF_CHECK_EQ(getsid(0), atoi(fields[3]));
	errno = 0;
	nice = getpriority(PRIO_PROCESS, 0);
	ATF_REQUIRE_EQ(0, errno);
	ATF_CHECK_EQ(20 + nice, atoi(fields[15]));
	ATF_CHECK_EQ(nice, atoi(fields[16]));
	ATF_CHECK_EQ(1, atoi(fields[17]));
	ATF_CHECK_EQ(SCHED_OTHER, atoi(fields[38]));
}

static int thread_pipe[2];
static volatile long thread_tid;

static void *
sleeper(void *arg __unused)
{
	char c;

	thread_tid = self_tid();
	(void)read(thread_pipe[0], &c, 1);
	return (NULL);
}

APROCFS_TC(task, "task lists the threads and each has its own stat");
ATF_TC_BODY(task, tc)
{
	char buf[2048], fields[60][32], name[32], path[64];
	pthread_t th;
	int i;

	mount_aprocfs();
	ATF_REQUIRE_EQ(0, pipe(thread_pipe));
	ATF_REQUIRE_EQ(0, pthread_create(&th, NULL, sleeper, NULL));
	for (i = 0; i < 1000 && thread_tid == 0; i++)
		usleep(1000);
	ATF_REQUIRE(thread_tid != 0);
	usleep(100000);

	snprintf(name, sizeof(name), "%ld", self_tid());
	ATF_CHECK(dir_has(MNT "/self/task", name));
	snprintf(name, sizeof(name), "%ld", thread_tid);
	ATF_CHECK(dir_has(MNT "/self/task", name));

	snprintf(path, sizeof(path), MNT "/self/task/%ld/stat", thread_tid);
	read_node(path, buf, sizeof(buf));
	ATF_CHECK_EQ(thread_tid, atol(buf));
	ATF_REQUIRE_EQ(50, stat_fields(buf, fields, 60));
	ATF_CHECK_STREQ("S", fields[0]);
	ATF_CHECK_EQ(2, atoi(fields[17]));

	snprintf(path, sizeof(path), MNT "/%ld/stat", thread_tid);
	read_node(path, buf, sizeof(buf));
	ATF_CHECK_EQ(thread_tid, atol(buf));

	ATF_REQUIRE_EQ(1, write(thread_pipe[1], "x", 1));
	ATF_REQUIRE_EQ(0, pthread_join(th, NULL));
}

APROCFS_TC(status, "status has the fields that libprocinfo reads");
ATF_TC_BODY(status, tc)
{
	char buf[4096], val[128], want[64];
	sigset_t set;

	mount_aprocfs();
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	ATF_REQUIRE_EQ(0, sigprocmask(SIG_BLOCK, &set, NULL));
	read_node(MNT "/self/status", buf, sizeof(buf));

	ATF_REQUIRE(status_field(buf, "Name:", val, sizeof(val)));
	ATF_REQUIRE(status_field(buf, "Tgid:", val, sizeof(val)));
	ATF_CHECK_EQ(getpid(), atoi(val));
	ATF_REQUIRE(status_field(buf, "Pid:", val, sizeof(val)));
	ATF_CHECK_EQ(getpid(), atoi(val));
	ATF_REQUIRE(status_field(buf, "PPid:", val, sizeof(val)));
	ATF_CHECK_EQ(getppid(), atoi(val));
	ATF_REQUIRE(status_field(buf, "TracerPid:", val, sizeof(val)));
	ATF_CHECK_EQ(0, atoi(val));
	ATF_REQUIRE(status_field(buf, "Uid:", val, sizeof(val)));
	snprintf(want, sizeof(want), "%u\t%u\t%u\t%u", getuid(), geteuid(),
	    geteuid(), geteuid());
	ATF_CHECK_STREQ(want, val);
	ATF_REQUIRE(status_field(buf, "Gid:", val, sizeof(val)));
	ATF_CHECK_EQ(getgid(), (gid_t)atoi(val));
	ATF_REQUIRE(status_field(buf, "Threads:", val, sizeof(val)));
	ATF_CHECK_EQ(1, atoi(val));
	ATF_REQUIRE(status_field(buf, "State:", val, sizeof(val)));
	ATF_CHECK_STREQ("R (running)", val);
	/* 128 signals as 32 hex digits; SIGUSR1 is bit SIGUSR1 - 1. */
	ATF_REQUIRE(status_field(buf, "SigBlk:", val, sizeof(val)));
	ATF_REQUIRE_EQ(32, strlen(val));
	ATF_CHECK((strtoul(val + 24, NULL, 16) & (1UL << (SIGUSR1 - 1))) != 0);
	ATF_REQUIRE(status_field(buf, "NoNewPrivs:", val, sizeof(val)));
	ATF_CHECK_STREQ("0", val);
	ATF_REQUIRE(status_field(buf, "Cpus_allowed_list:", val,
	    sizeof(val)));
	ATF_CHECK(val[0] >= '0' && val[0] <= '9');
}

APROCFS_TC(maps, "maps lines parse as libprocinfo parses them");
ATF_TC_BODY(maps, tc)
{
	static char buf[1024 * 1024];
	struct mapline ml;
	struct stat sb;
	char *line, *next, *path, *privmap, *shmap;
	bool found_priv, found_shared, found_stack, found_text;
	int fd, local;

	mount_aprocfs();
	fd = open("mapped", O_RDWR | O_CREAT, 0644);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE_EQ(0, ftruncate(fd, getpagesize()));
	ATF_REQUIRE_EQ(0, fstat(fd, &sb));
	shmap = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED,
	    fd, 0);
	ATF_REQUIRE(shmap != MAP_FAILED);
	privmap = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
	    MAP_PRIVATE, fd, 0);
	ATF_REQUIRE(privmap != MAP_FAILED);
	path = realpath("mapped", NULL);
	ATF_REQUIRE(path != NULL);

	read_node(MNT "/self/maps", buf, sizeof(buf));
	found_priv = found_shared = found_stack = found_text = false;
	for (line = buf; *line != '\0'; line = next) {
		next = strchr(line, '\n');
		ATF_REQUIRE(next != NULL);
		*next++ = '\0';
		parse_mapline(line, &ml);
		ATF_CHECK(ml.start < ml.end);
		if (ml.start == (unsigned long)shmap) {
			found_shared = true;
			ATF_CHECK_STREQ("rw-s", ml.perms);
			ATF_CHECK_STREQ(path, ml.name);
			ATF_CHECK_EQ(sb.st_ino, ml.inode);
		}
		if (ml.start == (unsigned long)privmap) {
			found_priv = true;
			ATF_CHECK_STREQ("rw-p", ml.perms);
			ATF_CHECK_STREQ(path, ml.name);
		}
		if ((unsigned long)&local >= ml.start &&
		    (unsigned long)&local < ml.end) {
			found_stack = true;
			ATF_CHECK_STREQ("[stack]", ml.name);
		}
		if ((unsigned long)mount_aprocfs >= ml.start &&
		    (unsigned long)mount_aprocfs < ml.end) {
			found_text = true;
			ATF_CHECK_EQ('x', ml.perms[2]);
			ATF_CHECK(ml.inode != 0 && ml.name[0] == '/');
		}
	}
	ATF_CHECK(found_shared);
	ATF_CHECK(found_priv);
	ATF_CHECK(found_stack);
	ATF_CHECK(found_text);
	free(path);
}

APROCFS_TC(cmdline, "cmdline is the kern.proc.args value");
ATF_TC_BODY(cmdline, tc)
{
	char buf[8192], want[8192];
	int mib[4];
	size_t len;
	ssize_t n;

	mount_aprocfs();
	n = read_node(MNT "/self/cmdline", buf, sizeof(buf));
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_ARGS;
	mib[3] = getpid();
	len = sizeof(want);
	ATF_REQUIRE_EQ(0, sysctl(mib, 4, want, &len, NULL, 0));
	ATF_REQUIRE_EQ((ssize_t)len, n);
	ATF_CHECK(memcmp(buf, want, len) == 0);
	ATF_CHECK_EQ('\0', buf[n - 1]);
}

APROCFS_TC(comm, "comm is the thread name, else the process name");
ATF_TC_BODY(comm, tc)
{
	char buf[64], path[64];

	mount_aprocfs();
	pthread_set_name_np(pthread_self(), "k22main");
	read_node(MNT "/self/comm", buf, sizeof(buf));
	ATF_CHECK_STREQ("k22main\n", buf);
	snprintf(path, sizeof(path), MNT "/self/task/%ld/comm", self_tid());
	read_node(path, buf, sizeof(buf));
	ATF_CHECK_STREQ("k22main\n", buf);
	/* Linux names have at most 15 bytes. */
	pthread_set_name_np(pthread_self(), "a-name-of-19-bytes");
	read_node(MNT "/self/comm", buf, sizeof(buf));
	ATF_CHECK_STREQ("a-name-of-19-by\n", buf);
}

APROCFS_TC(exe, "exe is the kern.proc.pathname value");
ATF_TC_BODY(exe, tc)
{
	char buf[PATH_MAX], want[PATH_MAX];
	int mib[4];
	size_t len;
	ssize_t n;

	mount_aprocfs();
	n = readlink(MNT "/self/exe", buf, sizeof(buf) - 1);
	ATF_REQUIRE(n > 0);
	buf[n] = '\0';
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PATHNAME;
	mib[3] = -1;
	len = sizeof(want);
	ATF_REQUIRE_EQ(0, sysctl(mib, 4, want, &len, NULL, 0));
	ATF_CHECK_STREQ(want, buf);
}

APROCFS_TC(fd, "fd has a link for each descriptor");
ATF_TC_BODY(fd, tc)
{
	char buf[PATH_MAX], name[32], path[64], *real, *want;
	struct stat sb;
	ssize_t n;
	int dfd, fd, p[2];

	mount_aprocfs();
	fd = open("file1", O_RDWR | O_CREAT, 0644);
	ATF_REQUIRE(fd >= 0);
	want = realpath("file1", NULL);
	ATF_REQUIRE(want != NULL);
	snprintf(name, sizeof(name), "%d", fd);
	ATF_CHECK(dir_has(MNT "/self/fd", name));

	/* A file with a path: a link that lookup follows (register X4). */
	snprintf(path, sizeof(path), MNT "/self/fd/%d", fd);
	n = readlink(path, buf, sizeof(buf) - 1);
	ATF_REQUIRE(n > 0);
	buf[n] = '\0';
	ATF_CHECK_STREQ(want, buf);
	ATF_REQUIRE_EQ(0, lstat(path, &sb));
	ATF_CHECK(S_ISLNK(sb.st_mode));
	real = realpath(path, NULL);
	ATF_REQUIRE(real != NULL);
	ATF_CHECK_STREQ(want, real);
	free(real);

	/* A removed file has no path. */
	ATF_REQUIRE_EQ(0, unlink("file1"));
	n = readlink(path, buf, sizeof(buf) - 1);
	ATF_REQUIRE(n > 0);
	buf[n] = '\0';
	ATF_CHECK_MSG(n > 10 && strcmp(buf + n - 10, " (deleted)") == 0,
	    "%s", buf);

	/* A pipe: open(2) of the node gives a duplicate. */
	ATF_REQUIRE_EQ(0, pipe(p));
	snprintf(path, sizeof(path), MNT "/self/fd/%d", p[1]);
	n = readlink(path, buf, sizeof(buf) - 1);
	ATF_REQUIRE(n > 0);
	buf[n] = '\0';
	ATF_CHECK_MSG(strncmp(buf, "pipe:[", 6) == 0, "%s", buf);
	dfd = open(path, O_WRONLY);
	ATF_REQUIRE_MSG(dfd >= 0, "open %s: %s", path, strerror(errno));
	ATF_REQUIRE_EQ(1, write(dfd, "z", 1));
	ATF_REQUIRE_EQ(1, read(p[0], buf, 1));
	ATF_CHECK_EQ('z', buf[0]);
	free(want);
}

APROCFS_TC(fsync, "fsync(2) and fdatasync(2) give EINVAL");
ATF_TC_BODY(fsync, tc)
{
	int fd;

	mount_aprocfs();
	fd = open(MNT "/version", O_RDONLY);
	ATF_REQUIRE(fd >= 0);
	ATF_CHECK_ERRNO(EINVAL, fsync(fd) == -1);
	ATF_CHECK_ERRNO(EINVAL, fdatasync(fd) == -1);
	close(fd);
}

APROCFS_TC(readonly, "The nodes cannot be written, removed or made");
ATF_TC_BODY(readonly, tc)
{

	mount_aprocfs();
	ATF_CHECK_ERRNO(EACCES, open(MNT "/version", O_WRONLY) == -1);
	ATF_CHECK(unlink(MNT "/version") == -1);
	ATF_CHECK(mkdir(MNT "/new", 0755) == -1);
	ATF_CHECK(open(MNT "/new", O_WRONLY | O_CREAT, 0644) == -1);
}

APROCFS_TC(jail, "A jailed reader sees only the processes of its jail");
ATF_TC_BODY(jail, tc)
{
	char cwd[PATH_MAX], mnt[PATH_MAX + 8], path[PATH_MAX + 32];
	struct stat sb;
	pid_t child, parent;
	int jid, status;

	mount_aprocfs();
	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	snprintf(mnt, sizeof(mnt), "%s/" MNT, cwd);
	jid = jail_setv(JAIL_CREATE, "path", "/", "persist", "true", NULL);
	ATF_REQUIRE_MSG(jid > 0, "jail_setv: %s", jail_errmsg);
	parent = getpid();
	child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		if (jail_attach(jid) != 0)
			_exit(10);
		snprintf(path, sizeof(path), "%s/%d", mnt, parent);
		if (stat(path, &sb) == 0 || errno != ENOENT)
			_exit(11);
		snprintf(path, sizeof(path), "%d", parent);
		if (dir_has(mnt, path))
			_exit(12);
		snprintf(path, sizeof(path), "%s/%d/stat", mnt, getpid());
		if (stat(path, &sb) != 0)
			_exit(13);
		_exit(0);
	}
	ATF_REQUIRE_EQ(child, waitpid(child, &status, 0));
	(void)jail_remove(jid);
	ATF_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child status %#x", status);
}

APROCFS_TC(candebug, "maps and fd need p_candebug(), stat needs p_cansee()");
ATF_TC_BODY(candebug, tc)
{
	char path[64];
	struct stat sb;
	size_t len;
	pid_t child, parent;
	int see, status;

	len = sizeof(see);
	ATF_REQUIRE_EQ(0, sysctlbyname("security.bsd.see_other_uids", &see,
	    &len, NULL, 0));
	if (see == 0)
		atf_tc_skip("security.bsd.see_other_uids is 0");
	mount_aprocfs();
	parent = getpid();
	child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		if (setgid(65534) != 0 || setuid(65534) != 0)
			_exit(10);
		proc_path(path, sizeof(path), parent, "maps");
		if (open(path, O_RDONLY) != -1 || errno != EACCES)
			_exit(11);
		proc_path(path, sizeof(path), parent, "fd");
		if (opendir(path) != NULL)
			_exit(12);
		proc_path(path, sizeof(path), parent, "stat");
		if (stat(path, &sb) != 0 || open(path, O_RDONLY) < 0)
			_exit(13);
		_exit(0);
	}
	ATF_REQUIRE_EQ(child, waitpid(child, &status, 0));
	ATF_CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "child status %#x", status);
}

APROCFS_TC(zombie, "A zombie shows state Z and its exit status");
ATF_TC_BODY(zombie, tc)
{
	char buf[2048], fields[60][32], path[64];
	pid_t child;
	int i, status;

	mount_aprocfs();
	child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0)
		_exit(7);
	proc_path(path, sizeof(path), child, "stat");
	for (i = 0; i < 1000; i++) {
		read_node(path, buf, sizeof(buf));
		ATF_REQUIRE_EQ(50, stat_fields(buf, fields, 60));
		if (strcmp(fields[0], "Z") == 0)
			break;
		usleep(1000);
	}
	ATF_CHECK_STREQ("Z", fields[0]);
	ATF_CHECK_EQ(7 << 8, atoi(fields[49]));
	proc_path(path, sizeof(path), child, "cmdline");
	ATF_CHECK_EQ(0, read_node(path, buf, sizeof(buf)));
	proc_path(path, sizeof(path), child, "exe");
	ATF_CHECK_ERRNO(ENOENT, readlink(path, buf, sizeof(buf)) == -1);
	ATF_REQUIRE_EQ(child, waitpid(child, &status, 0));
	ATF_CHECK_ERRNO(ENOENT, open(path, O_RDONLY) == -1);
}

APROCFS_TC(unload_busy, "The module does not unload while mounted");
ATF_TC_BODY(unload_busy, tc)
{
	int id;

	mount_aprocfs();
	id = kldfind("aprocfs.ko");
	if (id < 0)
		atf_tc_skip("aprocfs is not a loaded module");
	ATF_CHECK_ERRNO(EBUSY, kldunload(id) == -1);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, root);
	ATF_TP_ADD_TC(tp, self);
	ATF_TP_ADD_TC(tp, version);
	ATF_TP_ADD_TC(tp, pid_max);
	ATF_TP_ADD_TC(tp, stat);
	ATF_TP_ADD_TC(tp, task);
	ATF_TP_ADD_TC(tp, status);
	ATF_TP_ADD_TC(tp, maps);
	ATF_TP_ADD_TC(tp, cmdline);
	ATF_TP_ADD_TC(tp, comm);
	ATF_TP_ADD_TC(tp, exe);
	ATF_TP_ADD_TC(tp, fd);
	ATF_TP_ADD_TC(tp, fsync);
	ATF_TP_ADD_TC(tp, readonly);
	ATF_TP_ADD_TC(tp, jail);
	ATF_TP_ADD_TC(tp, candebug);
	ATF_TP_ADD_TC(tp, zombie);
	ATF_TP_ADD_TC(tp, unload_busy);
	return (atf_no_error());
}
