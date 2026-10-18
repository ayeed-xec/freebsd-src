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
 * asysfs: a /sys in the layout that Linux programs read, for the Android
 * jail.  See asysfs(5).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpuset.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/sbuf.h>
#include <sys/smp.h>

#include <fs/pseudofs/pseudofs.h>

static void	asysfs_cpulist(struct sbuf *sb, const cpuset_t *set);

/* The list format of cpuset(7), for example "0-3,6". */
static void
asysfs_cpulist(struct sbuf *sb, const cpuset_t *set)
{
	int cpu, first;
	bool any;

	any = false;
	for (cpu = 0; cpu <= (int)mp_maxid; cpu++) {
		if (!CPU_ISSET(cpu, set))
			continue;
		first = cpu;
		while (cpu < (int)mp_maxid && CPU_ISSET(cpu + 1, set))
			cpu++;
		sbuf_printf(sb, "%s%d", any ? "," : "", first);
		if (cpu != first)
			sbuf_printf(sb, "-%d", cpu);
		any = true;
	}
	sbuf_putc(sb, '\n');
}

/*
 * The CPUs that run threads.  FreeBSD has no CPU hot plug, so the present
 * CPUs are the same.  bionic-freebsd counts them through AT_NCPUS.
 */
static int
asysfs_doonline(PFS_FILL_ARGS)
{

	asysfs_cpulist(sb, &all_cpus);
	return (0);
}

/* Every CPU id that the kernel can use. */
static int
asysfs_dopossible(PFS_FILL_ARGS)
{

	sbuf_printf(sb, "0-%u\n", mp_maxid);
	return (0);
}

static int
asysfs_init(PFS_INIT_ARGS)
{
	struct pfs_node *cpu, *dir;
	int error;

	error = pfs_create_dir(pi->pi_root, &dir, "devices", NULL, NULL,
	    NULL, 0);
	if (error == 0)
		error = pfs_create_dir(dir, &dir, "system", NULL, NULL, NULL,
		    0);
	if (error == 0)
		error = pfs_create_dir(dir, &cpu, "cpu", NULL, NULL, NULL, 0);
	if (error == 0)
		error = pfs_create_file(cpu, NULL, "online", asysfs_doonline,
		    NULL, NULL, NULL, PFS_RD);
	if (error == 0)
		error = pfs_create_file(cpu, NULL, "possible",
		    asysfs_dopossible, NULL, NULL, NULL, PFS_RD);
	if (error == 0)
		error = pfs_create_file(cpu, NULL, "present", asysfs_doonline,
		    NULL, NULL, NULL, PFS_RD);
	return (error);
}

static int
asysfs_uninit(PFS_INIT_ARGS)
{

	return (0);
}

PSEUDOFS(asysfs, 1, VFCF_JAIL);
