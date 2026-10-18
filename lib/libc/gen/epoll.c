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

#include "namespace.h"
#include <sys/epoll.h>
#include <sys/specialfd.h>
#include <errno.h>
#include <stddef.h>
#include <time.h>
#include "un-namespace.h"
#include "libc_private.h"

int
epoll_create1(int flags)
{
	struct specialfd_epoll args;

	args.flags = flags;
	return (__sys___specialfd(SPECIALFD_EPOLL, &args, sizeof(args)));
}

int
epoll_create(int size)
{

	/* The size is only checked, as on Linux. */
	if (size <= 0) {
		errno = EINVAL;
		return (-1);
	}
	return (epoll_create1(0));
}

int
epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout,
    const sigset_t *sigmask)
{
	struct timespec ts, *tsp;

	/* Any negative timeout waits without a limit. */
	if (timeout >= 0) {
		ts.tv_sec = timeout / 1000;
		ts.tv_nsec = (timeout % 1000) * 1000000;
		tsp = &ts;
	} else
		tsp = NULL;
	return (__sys_epoll_pwait2(epfd, events, maxevents, tsp, sigmask));
}

int
epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{

	return (epoll_pwait(epfd, events, maxevents, timeout, NULL));
}
