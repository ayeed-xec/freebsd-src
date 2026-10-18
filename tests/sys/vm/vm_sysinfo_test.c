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
 * Tests of the vm.sysinfo sysctl: the load averages and the memory, swap and
 * process counts in one read.
 */

#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>

#include <vm/vm_param.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

static void
read_sysinfo(struct vmsysinfo *vs)
{
	size_t len;
	int mib[2];

	mib[0] = CTL_VM;
	mib[1] = VM_SYSINFO;
	len = sizeof(*vs);
	ATF_REQUIRE_MSG(sysctl(mib, nitems(mib), vs, &len, NULL, 0) == 0,
	    "vm.sysinfo: %s", strerror(errno));
	ATF_REQUIRE_EQ(sizeof(*vs), len);
}

static void
read_loadavg(struct loadavg *la)
{
	size_t len;
	int mib[2];

	mib[0] = CTL_VM;
	mib[1] = VM_LOADAVG;
	len = sizeof(*la);
	ATF_REQUIRE(sysctl(mib, nitems(mib), la, &len, NULL, 0) == 0);
}

/* The swap space of all devices in bytes, as swapinfo(8) counts it. */
static uint64_t
swap_space(void)
{
	struct xswdev xsw;
	uint64_t total;
	size_t len, miblen;
	int mib[16];

	miblen = nitems(mib);
	ATF_REQUIRE(sysctlnametomib("vm.swap_info", mib, &miblen) == 0);
	total = 0;
	for (mib[miblen] = 0;; mib[miblen]++) {
		len = sizeof(xsw);
		if (sysctl(mib, miblen + 1, &xsw, &len, NULL, 0) != 0) {
			ATF_REQUIRE_EQ(ENOENT, errno);
			break;
		}
		total += (uint64_t)xsw.xsw_nblks * getpagesize();
	}
	return (total);
}

ATF_TC_WITHOUT_HEAD(values);
ATF_TC_BODY(values, tc)
{
	struct loadavg la1, la2;
	struct vmsysinfo vs;
	unsigned long physmem;
	size_t len;
	int i, maxproc, tries;

	len = sizeof(physmem);
	ATF_REQUIRE(sysctlbyname("hw.physmem", &physmem, &len, NULL, 0) == 0);
	len = sizeof(maxproc);
	ATF_REQUIRE(sysctlbyname("kern.maxproc", &maxproc, &len, NULL, 0) ==
	    0);

	/* The load averages change every 5 seconds; read a stable copy. */
	for (tries = 0; tries < 10; tries++) {
		read_loadavg(&la1);
		read_sysinfo(&vs);
		read_loadavg(&la2);
		if (memcmp(&la1, &la2, sizeof(la1)) == 0)
			break;
	}
	ATF_REQUIRE(tries < 10);
	for (i = 0; i < 3; i++)
		ATF_REQUIRE_EQ(la1.ldavg[i], vs.vs_ldavg[i]);
	ATF_REQUIRE_EQ((uint32_t)la1.fscale, vs.vs_fscale);

	ATF_REQUIRE_EQ((uint64_t)physmem, vs.vs_physmem);
	ATF_REQUIRE(vs.vs_free > 0);
	ATF_REQUIRE(vs.vs_free <= vs.vs_physmem);
	ATF_REQUIRE_EQ(swap_space(), vs.vs_swaptotal);
	ATF_REQUIRE(vs.vs_swapfree <= vs.vs_swaptotal);
	ATF_REQUIRE(vs.vs_nprocs >= 1);
	ATF_REQUIRE(vs.vs_nprocs <= (uint32_t)maxproc);
	for (i = 0; i < (int)nitems(vs.vs_spare); i++)
		ATF_REQUIRE_EQ(0, vs.vs_spare[i]);
}

ATF_TC_WITHOUT_HEAD(size);
ATF_TC_BODY(size, tc)
{
	struct vmsysinfo vs;
	size_t len;
	int mib[2];

	ATF_REQUIRE_EQ(64, sizeof(struct vmsysinfo));
	mib[0] = CTL_VM;
	mib[1] = VM_SYSINFO;
	len = 0;
	ATF_REQUIRE(sysctl(mib, nitems(mib), NULL, &len, NULL, 0) == 0);
	ATF_REQUIRE_EQ(sizeof(struct vmsysinfo), len);

	len = 8;
	ATF_REQUIRE_ERRNO(ENOMEM,
	    sysctl(mib, nitems(mib), &vs, &len, NULL, 0) == -1);

	memset(&vs, 0, sizeof(vs));
	ATF_REQUIRE_ERRNO(EPERM,
	    sysctl(mib, nitems(mib), NULL, NULL, &vs, sizeof(vs)) == -1);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, values);
	ATF_TP_ADD_TC(tp, size);
	return (atf_no_error());
}
