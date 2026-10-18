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
 * Vnode operations of aprocfs.
 *
 * A vnode stands for a key (type, pid, tid, descriptor, flags) and holds
 * nothing else.  Vnodes are found through vfs_hash and recycled when they
 * become inactive, so a vnode lives only while someone uses it.  Lookups
 * bypass the name cache: the names under the root and under task/ and fd/
 * come and go with the processes.
 *
 * The content operations (read, readdir, readlink) unlock the vnode while
 * they work, as pseudofs does: they take the allproc lock, process locks
 * and descriptor table locks, may sleep, and copy out to user memory.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/hash.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sbuf.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <fs/aprocfs/aprocfs.h>

/* Names are pids, tids, descriptor numbers and the fixed names. */
#define	APROCFS_NAMELEN		16
#define	APROCFS_DIRENTSZ	_GENERIC_DIRLEN(APROCFS_NAMELEN)
/* The most entries that one readdir call returns. */
#define	APROCFS_DIRMAX		1024

struct aprocfs_ent {
	const char		*ae_name;
	enum aprocfs_type	 ae_type;
};

static const struct aprocfs_ent aprocfs_root_ents[] = {
	{ "self",	APROCFS_SELF },
	{ "sys",	APROCFS_SYS },
	{ "version",	APROCFS_VERSION },
	{ NULL,		APROCFS_NONE }
};

static const struct aprocfs_ent aprocfs_sys_ents[] = {
	{ "kernel",	APROCFS_SYSKERNEL },
	{ NULL,		APROCFS_NONE }
};

static const struct aprocfs_ent aprocfs_syskernel_ents[] = {
	{ "pid_max",	APROCFS_PIDMAX },
	{ NULL,		APROCFS_NONE }
};

/* A process directory.  A thread directory has the same without task. */
static const struct aprocfs_ent aprocfs_proc_ents[] = {
	{ "cmdline",	APROCFS_CMDLINE },
	{ "comm",	APROCFS_COMM },
	{ "exe",	APROCFS_EXE },
	{ "fd",		APROCFS_FDDIR },
	{ "maps",	APROCFS_MAPS },
	{ "stat",	APROCFS_STAT },
	{ "status",	APROCFS_STATUS },
	{ "task",	APROCFS_TASKDIR },
	{ NULL,		APROCFS_NONE }
};

/* The window of a readdir call, in entries. */
struct aprocfs_dirctx {
	const struct aprocfs_key *dc_key;	/* the directory */
	struct sbuf	*dc_sb;
	off_t		 dc_idx;
	off_t		 dc_first;
	int		 dc_count;
	int		 dc_max;
};

static vop_access_t	aprocfs_access;
static vop_getattr_t	aprocfs_getattr;
static vop_inactive_t	aprocfs_inactive;
static vop_lookup_t	aprocfs_lookup;
static vop_open_t	aprocfs_open;
static vop_pathconf_t	aprocfs_pathconf;
static vop_read_t	aprocfs_read;
static vop_readdir_t	aprocfs_readdir;
static vop_readlink_t	aprocfs_readlink;
static vop_reclaim_t	aprocfs_reclaim;

static int	aprocfs_child(struct thread *td,
		    const struct aprocfs_key *dkey, const char *name,
		    size_t len, struct aprocfs_key *key);
static int	aprocfs_check(enum aprocfs_type type);
static bool	aprocfs_dirent(struct aprocfs_dirctx *dc, const char *name,
		    uint8_t type, ino_t fileid);
static const struct aprocfs_ent *aprocfs_ents(const struct aprocfs_key *key);
static int	aprocfs_fd_cb(struct proc *p, int fd, struct file *fp,
		    void *arg);
static ino_t	aprocfs_fileid(const struct aprocfs_key *key);
static int	aprocfs_id(const char *name, size_t len, int *idp);
static void	aprocfs_parent(const struct aprocfs_key *key,
		    struct aprocfs_key *pkey);
static int	aprocfs_readdir_fill(struct thread *td,
		    const struct aprocfs_key *key, struct aprocfs_dirctx *dc);
static int	aprocfs_vcmp(struct vnode *vp, void *arg);
static int	aprocfs_vget_cb(struct mount *mp, void *arg, int lkflags,
		    struct vnode **vpp);
static __enum_uint8(vtype) aprocfs_vtype(const struct aprocfs_key *key);

struct vop_vector aprocfs_vnodeops = {
	.vop_default =		&default_vnodeops,

	.vop_access =		aprocfs_access,
	.vop_fdatasync =	VOP_EINVAL,
	.vop_fsync =		VOP_EINVAL,
	.vop_getattr =		aprocfs_getattr,
	.vop_inactive =		aprocfs_inactive,
	.vop_lookup =		aprocfs_lookup,
	.vop_open =		aprocfs_open,
	.vop_pathconf =		aprocfs_pathconf,
	.vop_read =		aprocfs_read,
	.vop_readdir =		aprocfs_readdir,
	.vop_readlink =		aprocfs_readlink,
	.vop_reclaim =		aprocfs_reclaim,
};
VFS_VOP_VECTOR_REGISTER(aprocfs_vnodeops);

static __enum_uint8(vtype)
aprocfs_vtype(const struct aprocfs_key *key)
{

	switch (key->ak_type) {
	case APROCFS_ROOT:
	case APROCFS_SYS:
	case APROCFS_SYSKERNEL:
	case APROCFS_PROC:
	case APROCFS_TASKDIR:
	case APROCFS_TASK:
	case APROCFS_FDDIR:
		return (VDIR);
	case APROCFS_SELF:
	case APROCFS_EXE:
		return (VLNK);
	case APROCFS_FD:
		return ((key->ak_flags & APROCFS_KF_ANON) != 0 ? VCHR : VLNK);
	default:
		return (VREG);
	}
}

/* The extra check of a node, after p_cansee(). */
static int
aprocfs_check(enum aprocfs_type type)
{

	switch (type) {
	case APROCFS_FDDIR:
	case APROCFS_FD:
	case APROCFS_MAPS:
		return (PGET_CANDEBUG);
	default:
		return (0);
	}
}

/*
 * A stable file number: the owner (thread or process) in the upper half,
 * the descriptor and the type in the lower half.
 */
static ino_t
aprocfs_fileid(const struct aprocfs_key *key)
{
	uint64_t owner;

	owner = key->ak_tid != 0 ? (uint64_t)key->ak_tid :
	    (uint64_t)key->ak_pid;
	return ((ino_t)(owner << 32 |
	    (uint64_t)(key->ak_type == APROCFS_FD ? key->ak_fd : 0) << 5 |
	    (uint64_t)key->ak_type));
}

static const struct aprocfs_ent *
aprocfs_ents(const struct aprocfs_key *key)
{

	switch (key->ak_type) {
	case APROCFS_ROOT:
		return (aprocfs_root_ents);
	case APROCFS_SYS:
		return (aprocfs_sys_ents);
	case APROCFS_SYSKERNEL:
		return (aprocfs_syskernel_ents);
	case APROCFS_PROC:
	case APROCFS_TASK:
		return (aprocfs_proc_ents);
	default:
		return (NULL);
	}
}

static void
aprocfs_parent(const struct aprocfs_key *key, struct aprocfs_key *pkey)
{

	memset(pkey, 0, sizeof(*pkey));
	switch (key->ak_type) {
	case APROCFS_SYSKERNEL:
		pkey->ak_type = APROCFS_SYS;
		break;
	case APROCFS_TASKDIR:
		pkey->ak_type = APROCFS_PROC;
		pkey->ak_pid = key->ak_pid;
		break;
	case APROCFS_TASK:
		pkey->ak_type = APROCFS_TASKDIR;
		pkey->ak_pid = key->ak_pid;
		break;
	case APROCFS_FDDIR:
		pkey->ak_type = (key->ak_flags & APROCFS_KF_TASK) != 0 ?
		    APROCFS_TASK : APROCFS_PROC;
		pkey->ak_pid = key->ak_pid;
		pkey->ak_tid = key->ak_tid;
		pkey->ak_flags = key->ak_flags & APROCFS_KF_TASK;
		break;
	default:
		pkey->ak_type = APROCFS_ROOT;
		break;
	}
}

static int
aprocfs_vcmp(struct vnode *vp, void *arg)
{
	const struct aprocfs_key *key;

	key = arg;
	return (memcmp(&VTOAPN(vp)->an_key, key, sizeof(*key)) != 0);
}

int
aprocfs_vget(struct mount *mp, const struct aprocfs_key *key, int lkflags,
    struct vnode **vpp)
{
	struct aprocfs_node *an;
	struct vnode *vp;
	u_int hash;
	int error;

	hash = hash32_buf(key, sizeof(*key), HASHINIT);
	error = vfs_hash_get(mp, hash, lkflags, curthread, vpp, aprocfs_vcmp,
	    __DECONST(void *, key));
	if (error != 0 || *vpp != NULL)
		return (error);

	an = malloc(sizeof(*an), M_APROCFS, M_WAITOK | M_ZERO);
	an->an_key = *key;
	error = getnewvnode("aprocfs", mp, &aprocfs_vnodeops, &vp);
	if (error != 0) {
		free(an, M_APROCFS);
		return (error);
	}
	vp->v_data = an;
	vp->v_type = aprocfs_vtype(key);
	if (key->ak_type == APROCFS_ROOT)
		vp->v_vflag |= VV_ROOT;
	if (vp->v_type == VCHR)
		vp->v_vflag |= VV_READLINK;
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	error = insmntque(vp, mp);
	if (error != 0) {
		free(an, M_APROCFS);
		*vpp = NULL;
		return (error);
	}
	vn_set_state(vp, VSTATE_CONSTRUCTED);
	error = vfs_hash_insert(vp, hash, lkflags, curthread, vpp, aprocfs_vcmp,
	    __DECONST(void *, key));
	if (error != 0 || *vpp != NULL)
		return (error);
	*vpp = vp;
	return (0);
}

static int
aprocfs_vget_cb(struct mount *mp, void *arg, int lkflags, struct vnode **vpp)
{

	return (aprocfs_vget(mp, arg, lkflags, vpp));
}

/* A decimal id without a sign or leading zeros. */
static int
aprocfs_id(const char *name, size_t len, int *idp)
{
	size_t i;
	long id;

	if (len == 0 || len > 10 || (name[0] == '0' && len > 1))
		return (ENOENT);
	id = 0;
	for (i = 0; i < len; i++) {
		if (name[i] < '0' || name[i] > '9')
			return (ENOENT);
		id = id * 10 + (name[i] - '0');
	}
	if (id > INT_MAX)
		return (ENOENT);
	*idp = (int)id;
	return (0);
}

/*
 * Find the key of the entry "name" in the directory dkey.  The caller has
 * checked that the reader may search the directory.
 */
static int
aprocfs_child(struct thread *td, const struct aprocfs_key *dkey,
    const char *name, size_t len, struct aprocfs_key *key)
{
	const struct aprocfs_ent *ae;
	struct thread *td2;
	struct proc *p;
	int error, id;
	bool anon;

	memset(key, 0, sizeof(*key));
	ae = aprocfs_ents(dkey);
	for (; ae != NULL && ae->ae_name != NULL; ae++) {
		if (ae->ae_type == APROCFS_TASKDIR && dkey->ak_tid != 0)
			continue;
		if (strlen(ae->ae_name) == len &&
		    memcmp(ae->ae_name, name, len) == 0) {
			key->ak_type = ae->ae_type;
			key->ak_pid = dkey->ak_pid;
			key->ak_tid = dkey->ak_tid;
			key->ak_flags = dkey->ak_flags & APROCFS_KF_TASK;
			return (0);
		}
	}

	if (aprocfs_id(name, len, &id) != 0)
		return (ENOENT);
	switch (dkey->ak_type) {
	case APROCFS_ROOT:
		/* A thread id resolves too, as on Linux, but is not listed. */
		if (id == 0 || pget(id, PGET_CANSEE, &p) != 0)
			return (ENOENT);
		key->ak_type = APROCFS_PROC;
		key->ak_pid = p->p_pid;
		key->ak_tid = id > PID_MAX ? id : 0;
		PROC_UNLOCK(p);
		return (0);
	case APROCFS_TASKDIR:
		error = aprocfs_getproc(td, dkey, 0, &p);
		if (error != 0)
			return (error);
		FOREACH_THREAD_IN_PROC(p, td2) {
			if (td2->td_tid == id)
				break;
		}
		PROC_UNLOCK(p);
		if (td2 == NULL)
			return (ENOENT);
		key->ak_type = APROCFS_TASK;
		key->ak_pid = dkey->ak_pid;
		key->ak_tid = id;
		key->ak_flags = APROCFS_KF_TASK;
		return (0);
	case APROCFS_FDDIR:
		error = aprocfs_getproc(td, dkey, PGET_CANDEBUG, &p);
		if (error != 0)
			return (error);
		_PHOLD(p);
		PROC_UNLOCK(p);
		error = aprocfs_fd_anon(td, p, id, &anon);
		PRELE(p);
		if (error != 0)
			return (error);
		key->ak_type = APROCFS_FD;
		key->ak_pid = dkey->ak_pid;
		key->ak_tid = dkey->ak_tid;
		key->ak_fd = id;
		key->ak_flags = (dkey->ak_flags & APROCFS_KF_TASK) |
		    (anon ? APROCFS_KF_ANON : 0);
		return (0);
	default:
		return (ENOENT);
	}
}

static int
aprocfs_lookup(struct vop_lookup_args *ap)
{
	struct componentname *cnp;
	struct aprocfs_key dkey, key;
	struct vnode *dvp;
	int error, locked;

	cnp = ap->a_cnp;
	dvp = ap->a_dvp;
	*ap->a_vpp = NULL;
	if (dvp->v_type != VDIR)
		return (ENOTDIR);
	if ((cnp->cn_flags & ISLASTCN) != 0 &&
	    (cnp->cn_nameiop == DELETE || cnp->cn_nameiop == RENAME))
		return (EOPNOTSUPP);
	error = VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, curthread);
	if (error != 0)
		return (error);

	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(dvp);
		*ap->a_vpp = dvp;
		return (0);
	}
	if ((cnp->cn_flags & ISDOTDOT) != 0) {
		aprocfs_parent(&VTOAPN(dvp)->an_key, &key);
		return (vn_vget_ino_gen(dvp, aprocfs_vget_cb, &key,
		    cnp->cn_lkflags, ap->a_vpp));
	}
	dkey = VTOAPN(dvp)->an_key;
	if (dkey.ak_type != APROCFS_FDDIR) {
		error = aprocfs_child(curthread, &dkey, cnp->cn_nameptr,
		    cnp->cn_namelen, &key);
	} else {
		/*
		 * The descriptor table lock is not taken under a vnode lock,
		 * so look the descriptor up with the directory unlocked.
		 */
		locked = VOP_ISLOCKED(dvp);
		vhold(dvp);
		VOP_UNLOCK(dvp);
		error = aprocfs_child(curthread, &dkey, cnp->cn_nameptr,
		    cnp->cn_namelen, &key);
		vn_lock(dvp, locked | LK_RETRY);
		vdrop(dvp);
		if (error == 0 && VN_IS_DOOMED(dvp))
			error = ENOENT;
	}
	if (error != 0)
		return (error);
	return (aprocfs_vget(dvp->v_mount, &key, cnp->cn_lkflags, ap->a_vpp));
}

static int
aprocfs_getattr(struct vop_getattr_args *ap)
{
	const struct aprocfs_key *key;
	struct vnode *vp;
	struct vattr *vap;
	struct timeval bt, tv;
	struct thread *td;
	struct proc *p;
	int error;

	vp = ap->a_vp;
	vap = ap->a_vap;
	key = &VTOAPN(vp)->an_key;
	td = curthread;

	vattr_null(vap);
	vap->va_type = vp->v_type;
	vap->va_nlink = vp->v_type == VDIR ? 2 : 1;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = aprocfs_fileid(key);
	vap->va_size = 0;
	vap->va_blocksize = PAGE_SIZE;
	vap->va_bytes = 0;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = NODEV;
	vap->va_filerev = 0;
	switch (vp->v_type) {
	case VDIR:
		vap->va_mode = key->ak_type == APROCFS_FDDIR ? 0500 : 0555;
		break;
	case VLNK:
		vap->va_mode = 0777;
		break;
	default:
		vap->va_mode = 0444;
		break;
	}
	getboottime(&bt);
	TIMEVAL_TO_TIMESPEC(&bt, &vap->va_mtime);

	if (key->ak_pid != 0) {
		error = aprocfs_getproc(td, key, 0, &p);
		if (error != 0)
			return (error);
		/* As Linux: a set-id process belongs to root. */
		if ((p->p_flag & P_SUGID) == 0) {
			vap->va_uid = p->p_ucred->cr_uid;
			vap->va_gid = p->p_ucred->cr_gid;
		}
		tv = p->p_stats->p_start;
		timevaladd(&tv, &bt);
		TIMEVAL_TO_TIMESPEC(&tv, &vap->va_mtime);
		PROC_UNLOCK(p);
		/*
		 * The mode of fd/<n> does not show the open mode: that needs
		 * the descriptor table lock, which is not taken under a vnode
		 * lock.
		 */
		if (key->ak_type == APROCFS_FD)
			vap->va_mode = S_IRWXU;
	}
	vap->va_atime = vap->va_ctime = vap->va_birthtime = vap->va_mtime;
	return (0);
}

static int
aprocfs_access(struct vop_access_args *ap)
{
	struct vattr va;
	int error;

	error = VOP_GETATTR(ap->a_vp, &va, ap->a_cred);
	if (error != 0)
		return (error);
	return (vaccess(ap->a_vp->v_type, va.va_mode, va.va_uid, va.va_gid,
	    ap->a_accmode, ap->a_cred));
}

static int
aprocfs_open(struct vop_open_args *ap)
{
	const struct aprocfs_key *key;
	struct vnode *vp;
	struct thread *td;
	struct proc *p;
	int error;

	vp = ap->a_vp;
	key = &VTOAPN(vp)->an_key;
	td = curthread;
	if (key->ak_pid != 0) {
		error = aprocfs_getproc(td, key, aprocfs_check(key->ak_type),
		    &p);
		if (error != 0)
			return (error);
		PROC_UNLOCK(p);
	}

	/*
	 * A descriptor with no path: open(2) duplicates it, as fdescfs(5)
	 * does.  There is no way to reach the file of another process.
	 */
	if (vp->v_type == VCHR) {
		if (key->ak_pid != td->td_proc->p_pid)
			return (EOPNOTSUPP);
		td->td_dupfd = key->ak_fd;
		return (ENODEV);
	}
	if ((ap->a_mode & FWRITE) != 0)
		return (EACCES);
	return (0);
}

static int
aprocfs_read(struct vop_read_args *ap)
{
	struct aprocfs_key key;
	struct vnode *vp;
	struct uio *uio;
	struct sbuf *sb;
	off_t limit;
	ssize_t len;
	int error, locked;

	vp = ap->a_vp;
	uio = ap->a_uio;
	if (vp->v_type == VDIR)
		return (EISDIR);
	if (vp->v_type != VREG)
		return (EINVAL);
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);
	key = VTOAPN(vp)->an_key;
	limit = uio->uio_offset + uio->uio_resid;

	locked = VOP_ISLOCKED(vp);
	vhold(vp);
	VOP_UNLOCK(vp);
	sb = sbuf_new_auto();
	error = aprocfs_fill(curthread, &key, sb, limit);
	if (sbuf_finish(sb) != 0 && error == 0)
		error = ENOMEM;
	if (error == 0 && uio->uio_offset < sbuf_len(sb)) {
		len = MIN(sbuf_len(sb) - uio->uio_offset, uio->uio_resid);
		error = uiomove(sbuf_data(sb) + uio->uio_offset, (int)len,
		    uio);
	}
	sbuf_delete(sb);
	vn_lock(vp, locked | LK_RETRY);
	vdrop(vp);
	return (error);
}

static int
aprocfs_readlink(struct vop_readlink_args *ap)
{
	struct aprocfs_key key;
	struct vnode *vp;
	struct sbuf *sb;
	int error, locked;

	vp = ap->a_vp;
	key = VTOAPN(vp)->an_key;
	locked = VOP_ISLOCKED(vp);
	vhold(vp);
	VOP_UNLOCK(vp);
	sb = sbuf_new_auto();
	error = aprocfs_link(curthread, &key, sb);
	if (sbuf_finish(sb) != 0 && error == 0)
		error = ENAMETOOLONG;
	if (error == 0)
		error = uiomove(sbuf_data(sb), (int)sbuf_len(sb), ap->a_uio);
	sbuf_delete(sb);
	vn_lock(vp, locked | LK_RETRY);
	vdrop(vp);
	return (error);
}

/*
 * Add one entry to the window of a readdir call.  The buffer was sized
 * before any lock was taken, so this does not sleep.  Returns false when
 * the window is full.
 */
static bool
aprocfs_dirent(struct aprocfs_dirctx *dc, const char *name, uint8_t type,
    ino_t fileid)
{
	struct dirent d;

	if (dc->dc_count >= dc->dc_max)
		return (false);
	if (dc->dc_idx++ < dc->dc_first)
		return (true);
	memset(&d, 0, APROCFS_DIRENTSZ);
	d.d_fileno = fileid;
	d.d_off = dc->dc_idx * APROCFS_DIRENTSZ;
	d.d_reclen = APROCFS_DIRENTSZ;
	d.d_type = type;
	d.d_namlen = strlcpy(d.d_name, name, APROCFS_NAMELEN + 1);
	sbuf_bcat(dc->dc_sb, &d, APROCFS_DIRENTSZ);
	dc->dc_count++;
	return (true);
}

static int
aprocfs_fd_cb(struct proc *p, int fd, struct file *fp __unused, void *arg)
{
	struct aprocfs_dirctx *dc;
	struct aprocfs_key key;
	char name[APROCFS_NAMELEN];

	dc = arg;
	memset(&key, 0, sizeof(key));
	key.ak_type = APROCFS_FD;
	key.ak_pid = p->p_pid;
	key.ak_tid = dc->dc_key->ak_tid;
	key.ak_fd = fd;
	snprintf(name, sizeof(name), "%d", fd);
	return (aprocfs_dirent(dc, name, DT_LNK, aprocfs_fileid(&key)) ?
	    0 : ENOSPC);
}

static int
aprocfs_readdir_fill(struct thread *td, const struct aprocfs_key *key,
    struct aprocfs_dirctx *dc)
{
	const struct aprocfs_ent *ae;
	struct aprocfs_key ckey, pkey;
	char name[APROCFS_NAMELEN];
	struct thread *td2;
	struct proc *p;
	int error;

	aprocfs_parent(key, &pkey);
	if (!aprocfs_dirent(dc, ".", DT_DIR, aprocfs_fileid(key)) ||
	    !aprocfs_dirent(dc, "..", DT_DIR, aprocfs_fileid(&pkey)))
		return (0);
	for (ae = aprocfs_ents(key); ae != NULL && ae->ae_name != NULL; ae++) {
		if (ae->ae_type == APROCFS_TASKDIR && key->ak_tid != 0)
			continue;
		memset(&ckey, 0, sizeof(ckey));
		ckey.ak_type = ae->ae_type;
		ckey.ak_pid = key->ak_pid;
		ckey.ak_tid = key->ak_tid;
		if (!aprocfs_dirent(dc, ae->ae_name,
		    IFTODT(VTTOIF(aprocfs_vtype(&ckey))),
		    aprocfs_fileid(&ckey)))
			return (0);
	}

	memset(&ckey, 0, sizeof(ckey));
	switch (key->ak_type) {
	case APROCFS_ROOT:
		ckey.ak_type = APROCFS_PROC;
		sx_slock(&allproc_lock);
		FOREACH_PROC_IN_SYSTEM(p) {
			PROC_LOCK(p);
			if (p->p_state == PRS_NEW || p->p_pid == 0 ||
			    p_cansee(td, p) != 0) {
				PROC_UNLOCK(p);
				continue;
			}
			ckey.ak_pid = p->p_pid;
			PROC_UNLOCK(p);
			snprintf(name, sizeof(name), "%d", ckey.ak_pid);
			if (!aprocfs_dirent(dc, name, DT_DIR,
			    aprocfs_fileid(&ckey)))
				break;
		}
		sx_sunlock(&allproc_lock);
		return (0);
	case APROCFS_TASKDIR:
		error = aprocfs_getproc(td, key, 0, &p);
		if (error != 0)
			return (error);
		ckey.ak_type = APROCFS_TASK;
		ckey.ak_pid = key->ak_pid;
		FOREACH_THREAD_IN_PROC(p, td2) {
			ckey.ak_tid = td2->td_tid;
			snprintf(name, sizeof(name), "%d", ckey.ak_tid);
			if (!aprocfs_dirent(dc, name, DT_DIR,
			    aprocfs_fileid(&ckey)))
				break;
		}
		PROC_UNLOCK(p);
		return (0);
	case APROCFS_FDDIR:
		error = aprocfs_getproc(td, key, PGET_CANDEBUG, &p);
		if (error != 0)
			return (error);
		_PHOLD(p);
		PROC_UNLOCK(p);
		error = fget_remote_foreach(td, p, aprocfs_fd_cb, dc);
		PRELE(p);
		/* ENOSPC: the window is full.  ENOENT: no descriptor table. */
		return (error == ENOSPC || error == ENOENT ? 0 : error);
	default:
		return (0);
	}
}

static int
aprocfs_readdir(struct vop_readdir_args *ap)
{
	struct aprocfs_dirctx dc;
	struct aprocfs_key key;
	struct vnode *vp;
	struct uio *uio;
	int error, locked;

	vp = ap->a_vp;
	uio = ap->a_uio;
	if (vp->v_type != VDIR)
		return (ENOTDIR);
	/* aprocfs cannot be exported, so nothing asks for cookies. */
	if (ap->a_ncookies != NULL)
		return (EOPNOTSUPP);
	if (uio->uio_offset < 0 || uio->uio_offset % APROCFS_DIRENTSZ != 0)
		return (EINVAL);
	if (uio->uio_resid < APROCFS_DIRENTSZ)
		return (EINVAL);
	key = VTOAPN(vp)->an_key;

	dc.dc_key = &key;
	dc.dc_idx = 0;
	dc.dc_first = uio->uio_offset / APROCFS_DIRENTSZ;
	dc.dc_count = 0;
	dc.dc_max = MIN(uio->uio_resid / APROCFS_DIRENTSZ, APROCFS_DIRMAX);
	dc.dc_sb = sbuf_new(NULL, NULL, dc.dc_max * APROCFS_DIRENTSZ,
	    SBUF_FIXEDLEN);

	locked = VOP_ISLOCKED(vp);
	vhold(vp);
	VOP_UNLOCK(vp);
	error = aprocfs_readdir_fill(curthread, &key, &dc);
	if (error == 0)
		error = sbuf_finish(dc.dc_sb);
	if (error == 0 && sbuf_len(dc.dc_sb) > 0)
		error = uiomove(sbuf_data(dc.dc_sb), (int)sbuf_len(dc.dc_sb),
		    uio);
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = error == 0 && dc.dc_count < dc.dc_max;
	sbuf_delete(dc.dc_sb);
	vn_lock(vp, locked | LK_RETRY);
	vdrop(vp);
	return (error);
}

static int
aprocfs_pathconf(struct vop_pathconf_args *ap)
{

	switch (ap->a_name) {
	case _PC_NAME_MAX:
		*ap->a_retval = APROCFS_NAMELEN;
		return (0);
	case _PC_LINK_MAX:
		*ap->a_retval = 2;
		return (0);
	default:
		return (vop_stdpathconf(ap));
	}
}

/* Nothing is kept in a vnode; drop it as soon as nobody uses it. */
static int
aprocfs_inactive(struct vop_inactive_args *ap)
{

	vrecycle(ap->a_vp);
	return (0);
}

static int
aprocfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp;

	vp = ap->a_vp;
	vfs_hash_remove(vp);
	free(vp->v_data, M_APROCFS);
	vp->v_data = NULL;
	return (0);
}
