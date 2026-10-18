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
 * Tests of PROT_BTI in mmap(2) and mprotect(2) on arm64.  A code page holds
 * "ret", or "bti c" and "ret".  A child process calls it through a function
 * pointer, which is an indirect branch, so the children also show that a
 * fork keeps the guarded pages.
 */

#include <sys/param.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <machine/elf.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

#define	INSN_BTI_C	0xd503245fU
#define	INSN_RET	0xd65f03c0U

/* Exit codes of the child of call_code(). */
#define	CALL_RETURNED	0
#define	CALL_BTI_FAULT	10
#define	CALL_OTHER_ILL	11

static void *volatile call_target;

static void
require_bti(void)
{
	u_long hwcap2;

	if (elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2)) != 0 ||
	    (hwcap2 & HWCAP2_BTI) == 0)
		atf_tc_skip("the CPU does not implement BTI");
}

static void
write_code(void *page, bool landing_pad)
{
	uint32_t *insn;

	insn = page;
	if (landing_pad)
		*insn++ = INSN_BTI_C;
	*insn++ = INSN_RET;
	__builtin___clear_cache(page, (char *)insn);
}

static void
sigill_handler(int sig __unused, siginfo_t *si, void *uc __unused)
{
	_exit(si->si_code == ILL_ILLOPC && si->si_addr == call_target ?
	    CALL_BTI_FAULT : CALL_OTHER_ILL);
}

/*
 * Call the code at addr in a child process.  Return CALL_RETURNED, or
 * CALL_BTI_FAULT for SIGILL with ILL_ILLOPC at addr.
 */
static int
call_code(void *addr)
{
	struct sigaction sa;
	pid_t pid;
	int status;

	pid = fork();
	ATF_REQUIRE(pid >= 0);
	if (pid == 0) {
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = sigill_handler;
		sa.sa_flags = SA_SIGINFO;
		if (sigaction(SIGILL, &sa, NULL) != 0)
			_exit(2);
		call_target = addr;
		((void (*)(void))call_target)();
		_exit(CALL_RETURNED);
	}
	ATF_REQUIRE_EQ(pid, waitpid(pid, &status, 0));
	ATF_REQUIRE_MSG(WIFEXITED(status), "child status %#x", status);
	return (WEXITSTATUS(status));
}

/* A read and write page with the code, then the given protection. */
static void *
code_page(bool landing_pad, int prot)
{
	void *p;

	p = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	write_code(p, landing_pad);
	ATF_REQUIRE(mprotect(p, getpagesize(), prot) == 0);
	return (p);
}

ATF_TC_WITHOUT_HEAD(mprotect_bti);
ATF_TC_BODY(mprotect_bti, tc)
{
	require_bti();
	ATF_REQUIRE_EQ(CALL_BTI_FAULT,
	    call_code(code_page(false, PROT_READ | PROT_EXEC | PROT_BTI)));
}

ATF_TC_WITHOUT_HEAD(mprotect_bti_landing_pad);
ATF_TC_BODY(mprotect_bti_landing_pad, tc)
{
	require_bti();
	ATF_REQUIRE_EQ(CALL_RETURNED,
	    call_code(code_page(true, PROT_READ | PROT_EXEC | PROT_BTI)));
}

ATF_TC_WITHOUT_HEAD(mprotect_no_bti);
ATF_TC_BODY(mprotect_no_bti, tc)
{
	require_bti();
	ATF_REQUIRE_EQ(CALL_RETURNED,
	    call_code(code_page(false, PROT_READ | PROT_EXEC)));
}

ATF_TC_WITHOUT_HEAD(mmap_bti);
ATF_TC_BODY(mmap_bti, tc)
{
	void *p;

	require_bti();
	p = mmap(NULL, getpagesize(),
	    PROT_READ | PROT_WRITE | PROT_EXEC | PROT_BTI,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	write_code(p, false);
	ATF_REQUIRE_EQ(CALL_BTI_FAULT, call_code(p));
	write_code(p, true);
	ATF_REQUIRE_EQ(CALL_RETURNED, call_code(p));
}

/* mprotect(2) without PROT_BTI does not remove the mark. */
ATF_TC_WITHOUT_HEAD(bti_stays);
ATF_TC_BODY(bti_stays, tc)
{
	void *p;

	require_bti();
	p = code_page(false, PROT_READ | PROT_EXEC | PROT_BTI);
	ATF_REQUIRE(mprotect(p, getpagesize(), PROT_READ | PROT_WRITE) == 0);
	write_code(p, false);
	ATF_REQUIRE(mprotect(p, getpagesize(), PROT_READ | PROT_EXEC) == 0);
	ATF_REQUIRE_EQ(CALL_BTI_FAULT, call_code(p));
}

/*
 * An untouched anonymous mapping has no VM object.  After munmap(2), a new
 * mapping at the same address must not be guarded.
 */
ATF_TC_WITHOUT_HEAD(munmap_untouched);
ATF_TC_BODY(munmap_untouched, tc)
{
	void *p, *q;

	require_bti();
	p = mmap(NULL, getpagesize(),
	    PROT_READ | PROT_WRITE | PROT_EXEC | PROT_BTI,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_REQUIRE(munmap(p, getpagesize()) == 0);
	q = mmap(p, getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0);
	ATF_REQUIRE(q == p);
	write_code(q, false);
	ATF_REQUIRE_EQ(CALL_RETURNED, call_code(q));
}

/* mprotect(2) skips a hole in the range; the hole must not be guarded. */
ATF_TC_WITHOUT_HEAD(hole_not_guarded);
ATF_TC_BODY(hole_not_guarded, tc)
{
	char *hole, *p;
	size_t ps;

	require_bti();
	ps = getpagesize();
	p = mmap(NULL, 3 * ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	hole = p + ps;
	ATF_REQUIRE(munmap(hole, ps) == 0);
	ATF_REQUIRE(mprotect(p, 3 * ps, PROT_READ | PROT_EXEC | PROT_BTI) ==
	    0);
	ATF_REQUIRE(mmap(hole, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0) == hole);
	write_code(hole, false);
	ATF_REQUIRE_EQ(CALL_RETURNED, call_code(hole));
}

/* These protections stay invalid. */
ATF_TC_WITHOUT_HEAD(einval);
ATF_TC_BODY(einval, tc)
{
	static const int bad[] = {
		0x08, 0x10, 0x20, PROT_MAX(PROT_BTI),
	};
	size_t i, ps;
	void *p;

	ps = getpagesize();
	p = mmap(NULL, ps, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	for (i = 0; i < nitems(bad); i++) {
		ATF_REQUIRE_ERRNO(EINVAL,
		    mprotect(p, ps, PROT_READ | bad[i]) == -1);
		ATF_REQUIRE_ERRNO(EINVAL, mmap(NULL, ps, PROT_READ | bad[i],
		    MAP_ANON | MAP_PRIVATE, -1, 0) == MAP_FAILED);
	}
	ATF_REQUIRE_ERRNO(EINVAL, mmap(NULL, ps, PROT_BTI, MAP_GUARD, -1, 0) ==
	    MAP_FAILED);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, mprotect_bti);
	ATF_TP_ADD_TC(tp, mprotect_bti_landing_pad);
	ATF_TP_ADD_TC(tp, mprotect_no_bti);
	ATF_TP_ADD_TC(tp, mmap_bti);
	ATF_TP_ADD_TC(tp, bti_stays);
	ATF_TP_ADD_TC(tp, munmap_untouched);
	ATF_TP_ADD_TC(tp, hole_not_guarded);
	ATF_TP_ADD_TC(tp, einval);
	return (atf_no_error());
}
