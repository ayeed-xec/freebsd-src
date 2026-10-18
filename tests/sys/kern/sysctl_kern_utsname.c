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
 * Tests of the kern.utsname sysctl: each field equals its own sysctl.
 */

#include <sys/param.h>
#include <sys/capsicum.h>
#include <sys/jail.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <errno.h>
#include <jail.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static const struct {
	const char	*name;
	size_t		off;
} fields[] = {
	{ "kern.ostype", offsetof(struct xutsname, xu_sysname) },
	{ "kern.hostname", offsetof(struct xutsname, xu_nodename) },
	{ "kern.osrelease", offsetof(struct xutsname, xu_release) },
	{ "kern.version", offsetof(struct xutsname, xu_version) },
	{ "hw.machine", offsetof(struct xutsname, xu_machine) },
	{ "kern.domainname", offsetof(struct xutsname, xu_domainname) },
};

static int
read_utsname(struct xutsname *xu)
{
	size_t len;
	int mib[2];

	mib[0] = CTL_KERN;
	mib[1] = KERN_UTSNAME;
	len = sizeof(*xu);
	if (sysctl(mib, nitems(mib), xu, &len, NULL, 0) != 0)
		return (-1);
	if (len != sizeof(*xu)) {
		errno = EINVAL;
		return (-1);
	}
	return (0);
}

/*
 * Check that a field holds the value, cut to fit, and NUL bytes after it.
 */
static void
check_field(const char *name, const char *field, const char *value)
{
	size_t i, n;

	n = strnlen(value, XUTSNAMELEN - 1);
	ATF_REQUIRE_MSG(memcmp(field, value, n) == 0,
	    "%s: \"%.*s\" is not \"%s\"", name, (int)n, field, value);
	for (i = n; i < XUTSNAMELEN; i++)
		ATF_REQUIRE_MSG(field[i] == '\0',
		    "%s: byte %zu after the value is not 0", name, i);
}

ATF_TC_WITHOUT_HEAD(fields);
ATF_TC_BODY(fields, tc)
{
	struct xutsname xu;
	char value[1024];
	size_t i, len;

	memset(&xu, 0xa5, sizeof(xu));
	ATF_REQUIRE_MSG(read_utsname(&xu) == 0, "kern.utsname: %s",
	    strerror(errno));
	for (i = 0; i < nitems(fields); i++) {
		len = sizeof(value);
		ATF_REQUIRE_MSG(sysctlbyname(fields[i].name, value, &len, NULL,
		    0) == 0, "%s: %s", fields[i].name, strerror(errno));
		check_field(fields[i].name, (char *)&xu + fields[i].off, value);
	}
}

ATF_TC_WITHOUT_HEAD(size);
ATF_TC_BODY(size, tc)
{
	struct xutsname xu;
	size_t len;
	int mib[2];

	mib[0] = CTL_KERN;
	mib[1] = KERN_UTSNAME;
	len = 0;
	ATF_REQUIRE(sysctl(mib, nitems(mib), NULL, &len, NULL, 0) == 0);
	ATF_REQUIRE_EQ(sizeof(struct xutsname), len);
	ATF_REQUIRE_EQ(6 * XUTSNAMELEN, len);

	len = 16;
	ATF_REQUIRE_ERRNO(ENOMEM,
	    sysctl(mib, nitems(mib), &xu, &len, NULL, 0) == -1);

	memset(&xu, 0, sizeof(xu));
	ATF_REQUIRE_ERRNO(EPERM,
	    sysctl(mib, nitems(mib), NULL, NULL, &xu, sizeof(xu)) == -1);
}

ATF_TC(jail);
ATF_TC_HEAD(jail, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(jail, tc)
{
	static const char host[] = "utsname.test";
	static const char domain[] = "utsname.domain";
	static const char release[] = "15.1-UTSNAME";
	struct xutsname xu;
	pid_t pid;
	int jid, status;

	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		jid = jail_setv(JAIL_CREATE | JAIL_ATTACH, "path", "/",
		    "host", "new", "host.hostname", host,
		    "host.domainname", domain, "osrelease", release, NULL);
		if (jid < 0)
			_exit(2);
		if (read_utsname(&xu) != 0)
			_exit(3);
		if (strcmp(xu.xu_nodename, host) != 0 ||
		    strcmp(xu.xu_domainname, domain) != 0 ||
		    strcmp(xu.xu_release, release) != 0)
			_exit(4);
		_exit(0);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_REQUIRE(WIFEXITED(status));
	ATF_REQUIRE_MSG(WEXITSTATUS(status) == 0, "child exit status %d",
	    WEXITSTATUS(status));
}

/* kern.version is not readable in capability mode, so neither is this. */
ATF_TC_WITHOUT_HEAD(capmode);
ATF_TC_BODY(capmode, tc)
{
	struct xutsname xu;
	pid_t pid;
	int status;

	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		if (cap_enter() != 0)
			_exit(errno == ENOSYS ? 0 : 2);
		if (read_utsname(&xu) == 0 || errno != EPERM)
			_exit(3);
		_exit(0);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_REQUIRE(WIFEXITED(status));
	ATF_REQUIRE_MSG(WEXITSTATUS(status) == 0, "child exit status %d",
	    WEXITSTATUS(status));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, fields);
	ATF_TP_ADD_TC(tp, size);
	ATF_TP_ADD_TC(tp, jail);
	ATF_TP_ADD_TC(tp, capmode);
	return (atf_no_error());
}
