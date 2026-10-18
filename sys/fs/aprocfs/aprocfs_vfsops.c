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
 * aprocfs: a /proc in the layout that Linux programs read, for the
 * Android jail.  See aprocfs(5).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <fs/aprocfs/aprocfs.h>

MALLOC_DEFINE(M_APROCFS, "aprocfs", "aprocfs nodes and buffers");

static vfs_mount_t	aprocfs_mount;
static vfs_root_t	aprocfs_root;
static vfs_statfs_t	aprocfs_statfs;
static vfs_unmount_t	aprocfs_unmount;

static const char *aprocfs_opts[] = {
	"from",
	NULL
};

static int
aprocfs_mount(struct mount *mp)
{

	if ((mp->mnt_flag & (MNT_UPDATE | MNT_ROOTFS)) != 0)
		return (EOPNOTSUPP);
	if (vfs_filteropt(mp->mnt_optnew, aprocfs_opts) != 0)
		return (EINVAL);
	MNT_ILOCK(mp);
	mp->mnt_kern_flag |= MNTK_LOOKUP_SHARED | MNTK_EXTENDED_SHARED;
	MNT_IUNLOCK(mp);
	vfs_getnewfsid(mp);
	vfs_mountedfrom(mp, "aprocfs");
	return (0);
}

static int
aprocfs_unmount(struct mount *mp, int mntflags)
{

	return (vflush(mp, 0, (mntflags & MNT_FORCE) != 0 ? FORCECLOSE : 0,
	    curthread));
}

static int
aprocfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct aprocfs_key key;

	memset(&key, 0, sizeof(key));
	key.ak_type = APROCFS_ROOT;
	return (aprocfs_vget(mp, &key, flags, vpp));
}

static int
aprocfs_statfs(struct mount *mp, struct statfs *sbp)
{

	sbp->f_bsize = PAGE_SIZE;
	sbp->f_iosize = PAGE_SIZE;
	sbp->f_blocks = 0;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 0;
	sbp->f_ffree = 0;
	return (0);
}

static struct vfsops aprocfs_vfsops = {
	.vfs_mount =		aprocfs_mount,
	.vfs_root =		aprocfs_root,
	.vfs_statfs =		aprocfs_statfs,
	.vfs_unmount =		aprocfs_unmount,
};

VFS_SET(aprocfs_vfsops, aprocfs, VFCF_SYNTHETIC | VFCF_JAIL);
MODULE_VERSION(aprocfs, 1);
