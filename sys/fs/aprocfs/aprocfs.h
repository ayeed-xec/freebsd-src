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

#ifndef _FS_APROCFS_APROCFS_H_
#define	_FS_APROCFS_APROCFS_H_

#ifdef _KERNEL

MALLOC_DECLARE(M_APROCFS);

/*
 * Node types.  The vnode of a node is identified by its key: the type, the
 * process, the thread, the descriptor and the flags.  No other state is
 * kept; each operation looks the process up again.
 */
enum aprocfs_type {
	APROCFS_NONE = 0,
	APROCFS_ROOT,		/* / */
	APROCFS_SELF,		/* /self */
	APROCFS_VERSION,	/* /version */
	APROCFS_SYS,		/* /sys */
	APROCFS_SYSKERNEL,	/* /sys/kernel */
	APROCFS_PIDMAX,		/* /sys/kernel/pid_max */
	APROCFS_PROC,		/* /<pid>, or /<tid> with ak_tid set */
	APROCFS_TASKDIR,	/* /<pid>/task */
	APROCFS_TASK,		/* /<pid>/task/<tid> */
	APROCFS_FDDIR,		/* fd */
	APROCFS_FD,		/* fd/<n> */
	APROCFS_CMDLINE,	/* cmdline */
	APROCFS_COMM,		/* comm */
	APROCFS_EXE,		/* exe */
	APROCFS_MAPS,		/* maps */
	APROCFS_STAT,		/* stat */
	APROCFS_STATUS,		/* status */
	APROCFS_NTYPES
};

/* ak_flags */
#define	APROCFS_KF_TASK	0x0001	/* below task/<tid> */
#define	APROCFS_KF_ANON	0x0002	/* fd/<n> of a file with no path */

struct aprocfs_key {
	enum aprocfs_type	ak_type;
	pid_t			ak_pid;		/* 0 for a global node */
	lwpid_t			ak_tid;		/* 0 for the process view */
	int			ak_fd;		/* fd/<n> only */
	int			ak_flags;
};

struct aprocfs_node {
	struct aprocfs_key	an_key;
};

#define	VTOAPN(vp)	((struct aprocfs_node *)(vp)->v_data)

extern struct vop_vector aprocfs_vnodeops;

int	aprocfs_fd_anon(struct thread *td, struct proc *p, int fd,
	    bool *anonp);
int	aprocfs_fill(struct thread *td, const struct aprocfs_key *key,
	    struct sbuf *sb, off_t limit);
int	aprocfs_getproc(struct thread *td, const struct aprocfs_key *key,
	    int flags, struct proc **pp);
int	aprocfs_link(struct thread *td, const struct aprocfs_key *key,
	    struct sbuf *sb);
int	aprocfs_vget(struct mount *mp, const struct aprocfs_key *key,
	    int lkflags, struct vnode **vpp);

#endif /* _KERNEL */

#endif /* !_FS_APROCFS_APROCFS_H_ */
