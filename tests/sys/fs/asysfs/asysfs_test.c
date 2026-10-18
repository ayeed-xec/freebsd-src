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
 * Tests of asysfs(5).  The CPU lists are parsed as bionic's
 * sysconf(_SC_NPROCESSORS_ONLN) test parses them.
 */

#include <sys/param.h>
#include <sys/linker.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <mntopts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	MNT	"mnt"
#define	CPU	MNT "/devices/system/cpu"

static void
mount_asysfs(void)
{
	struct iovec *iov;
	char errmsg[256];
	int error, iovlen;

	ATF_REQUIRE_EQ(0, mkdir(MNT, 0755));
	iov = NULL;
	iovlen = 0;
	build_iovec(&iov, &iovlen, __DECONST(char *, "fstype"),
	    __DECONST(char *, "asysfs"), (size_t)-1);
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

static void
read_node(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	ATF_REQUIRE_MSG(fd >= 0, "open %s: %s", path, strerror(errno));
	n = read(fd, buf, size - 1);
	ATF_REQUIRE(n > 0);
	buf[n] = '\0';
	close(fd);
}

/* Count the CPUs of a list such as "0-3,6". */
static int
count_cpus(const char *list)
{
	const char *p;
	char *end;
	long first, last;
	int count;

	count = 0;
	for (p = list; *p != '\0' && *p != '\n';) {
		first = strtol(p, &end, 10);
		ATF_REQUIRE(end != p);
		last = first;
		if (*end == '-') {
			p = end + 1;
			last = strtol(p, &end, 10);
			ATF_REQUIRE(end != p && last >= first);
		}
		count += last - first + 1;
		p = end;
		if (*p == ',')
			p++;
	}
	ATF_REQUIRE_EQ('\n', *p);
	return (count);
}

#define	ASYSFS_TC(name, description)					\
	ATF_TC_WITH_CLEANUP(name);					\
	ATF_TC_HEAD(name, tc)						\
	{								\
		atf_tc_set_md_var(tc, "descr", description);		\
		atf_tc_set_md_var(tc, "require.user", "root");		\
		atf_tc_set_md_var(tc, "require.kmods", "asysfs");	\
		atf_tc_set_md_var(tc, "timeout", "30");			\
	}								\
	ATF_TC_CLEANUP(name, tc)					\
	{								\
		(void)unmount(MNT, MNT_FORCE);				\
	}

ASYSFS_TC(cpu_online, "online lists hw.ncpu CPUs");
ASYSFS_TC(cpu_present, "present is the same as online");
ASYSFS_TC(cpu_possible, "possible is 0 to kern.smp.maxid");
ASYSFS_TC(readonly, "The nodes cannot be written");
ASYSFS_TC(unload_busy, "The module does not unload while mounted");

ATF_TC_BODY(cpu_online, tc)
{
	char buf[256];
	size_t len;
	int ncpu;

	mount_asysfs();
	read_node(CPU "/online", buf, sizeof(buf));
	len = sizeof(ncpu);
	ATF_REQUIRE_EQ(0, sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0));
	ATF_CHECK_EQ(ncpu, count_cpus(buf));
}

ATF_TC_BODY(cpu_present, tc)
{
	char online[256], present[256];

	mount_asysfs();
	read_node(CPU "/online", online, sizeof(online));
	read_node(CPU "/present", present, sizeof(present));
	ATF_CHECK_STREQ(online, present);
}

ATF_TC_BODY(cpu_possible, tc)
{
	char buf[256], want[64];
	size_t len;
	int maxid;

	mount_asysfs();
	read_node(CPU "/possible", buf, sizeof(buf));
	len = sizeof(maxid);
	ATF_REQUIRE_EQ(0, sysctlbyname("kern.smp.maxid", &maxid, &len, NULL,
	    0));
	snprintf(want, sizeof(want), "0-%d\n", maxid);
	ATF_CHECK_STREQ(want, buf);
}

ATF_TC_BODY(readonly, tc)
{
	int fd;

	mount_asysfs();
	/* pseudofs lets root open a node for writing; the write fails. */
	fd = open(CPU "/online", O_WRONLY);
	if (fd >= 0) {
		ATF_CHECK(write(fd, "0\n", 2) == -1);
		close(fd);
	}
	ATF_CHECK(unlink(CPU "/online") == -1);
}

ATF_TC_BODY(unload_busy, tc)
{
	int id;

	mount_asysfs();
	id = kldfind("asysfs.ko");
	if (id < 0)
		atf_tc_skip("asysfs is not a loaded module");
	ATF_CHECK_ERRNO(EBUSY, kldunload(id) == -1);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, cpu_online);
	ATF_TP_ADD_TC(tp, cpu_present);
	ATF_TP_ADD_TC(tp, cpu_possible);
	ATF_TP_ADD_TC(tp, readonly);
	ATF_TP_ADD_TC(tp, unload_busy);
	return (atf_no_error());
}
