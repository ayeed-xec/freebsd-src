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
 * The content of the aprocfs nodes.  Every value comes from the functions
 * behind the kern.proc sysctls, so a node shows what procstat(1) shows, in
 * the layout that the Linux readers of Android expect.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/cpuset.h>
#include <sys/exec.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/jail.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/rtprio.h>
#include <sys/sbuf.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/ucred.h>
#include <sys/user.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#include <fs/aprocfs/aprocfs.h>

/* bionic-freebsd counts clock ticks in Android's HZ. */
#define	APROCFS_CLK_TCK		100
/* The longest name that Linux gives a task. */
#define	APROCFS_COMMLEN		16
/* The column where the name starts in a maps line, as on Linux. */
#define	APROCFS_MAPS_NAMECOL	72

CTASSERT(sizeof(struct kinfo_proc) % sizeof(uint64_t) == 0);
CTASSERT(sizeof(struct kinfo_vmentry) % sizeof(uint64_t) == 0);

/*
 * What stat and status show for a process or a thread.  The kinfo_proc
 * part is what kern.proc.pid gives; the other fields need the process lock
 * and kinfo_proc has no room for them.
 */
struct aprocfs_pinfo {
	struct kinfo_proc	 pi_ki;
	struct rusage		 pi_cru;
	sigset_t		 pi_shdpnd;
	sigset_t		 pi_thrpnd;
	cpuset_t		 pi_cpus;
	struct ucred		*pi_cred;
	rlim_t			 pi_rsslim;
	vm_offset_t		 pi_startcode;
	vm_offset_t		 pi_endcode;
	vm_offset_t		 pi_startstack;
	vm_offset_t		 pi_startdata;
	vm_offset_t		 pi_enddata;
	pid_t			 pi_ppid;
	pid_t			 pi_tracer;
	u_int			 pi_umask;
	int			 pi_sigparent;
	bool			 pi_candebug;
	bool			 pi_nonewprivs;
};

struct aprocfs_tdpick {
	struct kinfo_proc	*tp_ki;
	lwpid_t			 tp_tid;
	bool			 tp_found;
};

struct aprocfs_mapsctx {
	struct sbuf		*mc_out;
	off_t			 mc_limit;
	vm_offset_t		 mc_shp;
	bool			 mc_stopped;
};

static void	aprocfs_comm(const struct kinfo_proc *ki, char *buf,
		    size_t len);
static void	aprocfs_cpulist(struct sbuf *sb, const cpuset_t *set);
static void	aprocfs_cpumask(struct sbuf *sb, const cpuset_t *set);
static int	aprocfs_do_cmdline(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb);
static int	aprocfs_do_comm(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb);
static int	aprocfs_do_exe(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb);
static int	aprocfs_do_fd(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb);
static int	aprocfs_do_maps(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb,
		    off_t limit);
static int	aprocfs_do_pidmax(struct thread *td, struct sbuf *sb);
static int	aprocfs_do_stat(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb);
static int	aprocfs_do_status(struct thread *td,
		    const struct aprocfs_key *key, struct sbuf *sb);
static int	aprocfs_do_version(struct thread *td, struct sbuf *sb);
static int	aprocfs_maps_drain(void *arg, const char *data, int len);
static void	aprocfs_maps_line(struct aprocfs_mapsctx *mc,
		    const struct kinfo_vmentry *kve);
static int	aprocfs_pinfo_get(struct thread *td,
		    const struct aprocfs_key *key, struct aprocfs_pinfo *pi);
static void	aprocfs_sched(const struct kinfo_proc *ki, int *prio,
		    u_int *rtprio, u_int *policy);
static pid_t	aprocfs_seepid(struct thread *td, pid_t pid);
static void	aprocfs_sigset(struct sbuf *sb, const sigset_t *set);
static uint64_t	aprocfs_sig64(const sigset_t *set);
static char	aprocfs_state(const struct kinfo_proc *ki);
static const char *aprocfs_state_name(char state);
static int	aprocfs_tdpick_drain(void *arg, const char *data, int len);
static uint64_t	aprocfs_ticks(const struct timeval *tv);

/*
 * Find the process of a node and check that the reader can see it, and,
 * with PGET_CANDEBUG, debug it.  For a thread node, check that the thread
 * is still there.  Returns the process locked.  A process that the reader
 * cannot see gives ENOENT, as if it did not exist.
 */
int
aprocfs_getproc(struct thread *td, const struct aprocfs_key *key, int flags,
    struct proc **pp)
{
	struct thread *td2;
	struct proc *p;

	MPASS(td == curthread);
	if (pget(key->ak_pid, PGET_CANSEE | PGET_NOTID, &p) != 0)
		return (ENOENT);
	if (key->ak_tid != 0) {
		FOREACH_THREAD_IN_PROC(p, td2) {
			if (td2->td_tid == key->ak_tid)
				break;
		}
		if (td2 == NULL) {
			PROC_UNLOCK(p);
			return (ENOENT);
		}
	}
	if ((flags & PGET_CANDEBUG) != 0 && p_candebug(td, p) != 0) {
		PROC_UNLOCK(p);
		return (EACCES);
	}
	*pp = p;
	return (0);
}

int
aprocfs_fill(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb, off_t limit)
{

	switch (key->ak_type) {
	case APROCFS_VERSION:
		return (aprocfs_do_version(td, sb));
	case APROCFS_PIDMAX:
		return (aprocfs_do_pidmax(td, sb));
	case APROCFS_CMDLINE:
		return (aprocfs_do_cmdline(td, key, sb));
	case APROCFS_COMM:
		return (aprocfs_do_comm(td, key, sb));
	case APROCFS_MAPS:
		return (aprocfs_do_maps(td, key, sb, limit));
	case APROCFS_STAT:
		return (aprocfs_do_stat(td, key, sb));
	case APROCFS_STATUS:
		return (aprocfs_do_status(td, key, sb));
	default:
		return (EINVAL);
	}
}

int
aprocfs_link(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{

	switch (key->ak_type) {
	case APROCFS_SELF:
		sbuf_printf(sb, "%d", td->td_proc->p_pid);
		return (0);
	case APROCFS_EXE:
		return (aprocfs_do_exe(td, key, sb));
	case APROCFS_FD:
		return (aprocfs_do_fd(td, key, sb));
	default:
		return (EINVAL);
	}
}

static int
aprocfs_do_version(struct thread *td, struct sbuf *sb)
{
	char ostype[32], release[OSRELEASELEN];
	char *version;
	size_t len;
	int error;

	version = malloc(MAXPATHLEN, M_APROCFS, M_WAITOK);
	len = sizeof(ostype);
	error = kernel_sysctlbyname(td, "kern.ostype", ostype, &len, NULL, 0,
	    NULL, 0);
	if (error != 0)
		goto out;
	len = sizeof(release);
	error = kernel_sysctlbyname(td, "kern.osrelease", release, &len,
	    NULL, 0, NULL, 0);
	if (error != 0)
		goto out;
	len = MAXPATHLEN;
	error = kernel_sysctlbyname(td, "kern.version", version, &len, NULL,
	    0, NULL, 0);
	if (error != 0)
		goto out;
	version[MAXPATHLEN - 1] = '\0';
	len = strlen(version);
	while (len > 0 && version[len - 1] == '\n')
		version[--len] = '\0';
	sbuf_printf(sb, "%s version %s (%s)\n", ostype, release, version);
out:
	free(version, M_APROCFS);
	return (error);
}

/*
 * Linux defines pid_max as one more than the largest id.  FreeBSD thread
 * ids go up to NO_PID + kern.maxthread - 1, above every process id.
 */
static int
aprocfs_do_pidmax(struct thread *td, struct sbuf *sb)
{
	size_t len;
	int error, maxthread;

	len = sizeof(maxthread);
	error = kernel_sysctlbyname(td, "kern.maxthread", &maxthread, &len,
	    NULL, 0, NULL, 0);
	if (error != 0)
		return (error);
	sbuf_printf(sb, "%d\n", NO_PID + maxthread);
	return (0);
}

static int
aprocfs_do_cmdline(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{
	struct pargs *pa;
	struct proc *p;
	int error;

	error = aprocfs_getproc(td, key, 0, &p);
	if (error != 0)
		return (error);
	/* As on Linux, a zombie has no arguments. */
	pa = p->p_state == PRS_NORMAL ? p->p_args : NULL;
	if (pa != NULL) {
		pargs_hold(pa);
		PROC_UNLOCK(p);
		sbuf_bcat(sb, pa->ar_args, pa->ar_length);
		pargs_drop(pa);
	} else if (p->p_state == PRS_NORMAL &&
	    (p->p_flag & (P_WEXIT | P_SYSTEM)) == 0) {
		_PHOLD(p);
		PROC_UNLOCK(p);
		error = proc_getargv(td, p, sb);
		PRELE(p);
	} else
		PROC_UNLOCK(p);
	return (error);
}

static int
aprocfs_do_comm(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{
	char name[APROCFS_COMMLEN];
	struct thread *td2;
	struct proc *p;
	int error;

	error = aprocfs_getproc(td, key, 0, &p);
	if (error != 0)
		return (error);
	if (key->ak_tid == 0)
		td2 = FIRST_THREAD_IN_PROC(p);
	else {
		FOREACH_THREAD_IN_PROC(p, td2) {
			if (td2->td_tid == key->ak_tid)
				break;
		}
	}
	if (td2 != NULL && td2->td_name[0] != '\0')
		strlcpy(name, td2->td_name, sizeof(name));
	else
		strlcpy(name, p->p_comm, sizeof(name));
	PROC_UNLOCK(p);
	sbuf_printf(sb, "%s\n", name);
	return (0);
}

static int
aprocfs_do_exe(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{
	char *binname, *fullpath, *freepath;
	struct proc *p;
	int error;

	binname = malloc(MAXPATHLEN, M_APROCFS, M_WAITOK | M_ZERO);
	error = aprocfs_getproc(td, key, 0, &p);
	if (error != 0) {
		free(binname, M_APROCFS);
		return (error);
	}
	error = proc_get_binpath(p, binname, &fullpath, &freepath);
	if (error == 0) {
		if (fullpath[0] == '\0')
			error = ENOENT;
		else
			sbuf_cat(sb, fullpath);
	}
	free(freepath, M_TEMP);
	free(binname, M_APROCFS);
	return (error);
}

/*
 * Decide the vnode type of fd/<n> at lookup.  A file with a path is a
 * symbolic link to that path.  Every other descriptor is a node that
 * readlink(2) reads and that open(2) duplicates, as fdescfs(5) does.
 * The caller holds the process; it is not locked.
 */
int
aprocfs_fd_anon(struct thread *td, struct proc *p, int fd, bool *anonp)
{
	char *fullpath, *freepath;
	struct file *fp;

	if (fget_remote(td, p, fd, &fp) != 0)
		return (ENOENT);
	*anonp = true;
	if ((fp->f_type == DTYPE_VNODE || fp->f_type == DTYPE_FIFO) &&
	    fp->f_vnode != NULL) {
		freepath = NULL;
		if (vn_fullpath(fp->f_vnode, &fullpath, &freepath) == 0)
			*anonp = false;
		free(freepath, M_TEMP);
	}
	fdrop(fp, td);
	return (0);
}

static int
aprocfs_do_fd(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{
	struct kinfo_file *kif;
	char *fullpath, *freepath;
	struct vattr va;
	struct stat st;
	struct file *fp;
	struct proc *p;
	struct vnode *vp;
	int error;

	error = aprocfs_getproc(td, key, PGET_CANDEBUG, &p);
	if (error != 0)
		return (error);
	_PHOLD(p);
	PROC_UNLOCK(p);
	error = fget_remote(td, p, key->ak_fd, &fp);
	PRELE(p);
	if (error != 0)
		return (ENOENT);

	vp = fp->f_vnode;
	switch (fp->f_type) {
	case DTYPE_VNODE:
	case DTYPE_FIFO:
		if (vp == NULL)
			goto unknown;
		freepath = NULL;
		if (vn_fullpath(vp, &fullpath, &freepath) == 0) {
			sbuf_cat(sb, fullpath);
			free(freepath, M_TEMP);
			break;
		}
		vn_lock(vp, LK_SHARED | LK_RETRY);
		error = VOP_GETATTR(vp, &va, td->td_ucred);
		VOP_UNLOCK(vp);
		if (error != 0)
			break;
		sbuf_printf(sb, "/#%ju%s", (uintmax_t)va.va_fileid,
		    va.va_nlink == 0 ? " (deleted)" : "");
		break;
	case DTYPE_PIPE:
	case DTYPE_SOCKET:
		error = fo_stat(fp, &st, td->td_ucred);
		if (error != 0)
			break;
		sbuf_printf(sb, "%s:[%ju]",
		    fp->f_type == DTYPE_PIPE ? "pipe" : "socket",
		    (uintmax_t)st.st_ino);
		break;
	case DTYPE_SHM:
		kif = malloc(sizeof(*kif), M_APROCFS, M_WAITOK | M_ZERO);
		error = fo_fill_kinfo(fp, kif, NULL);
		if (error == 0) {
			/*
			 * A named object has a path that starts with "/".  An
			 * anonymous one shows the name that memfd_create(3)
			 * gave it ("memfd:<name>"), if any.
			 */
			if (kif->kf_path[0] == '/')
				sbuf_cat(sb, kif->kf_path);
			else if (kif->kf_path[0] != '\0')
				sbuf_printf(sb, "/%s (deleted)", kif->kf_path);
			else
				sbuf_cat(sb, "/memfd: (deleted)");
		}
		free(kif, M_APROCFS);
		break;
	case DTYPE_KQUEUE:
		sbuf_cat(sb, "anon_inode:[kqueue]");
		break;
	case DTYPE_EVENTFD:
		sbuf_cat(sb, "anon_inode:[eventfd]");
		break;
	case DTYPE_TIMERFD:
		sbuf_cat(sb, "anon_inode:[timerfd]");
		break;
	case DTYPE_INOTIFY:
		sbuf_cat(sb, "anon_inode:inotify");
		break;
	case DTYPE_PROCDESC:
		sbuf_cat(sb, "anon_inode:[pidfd]");
		break;
	case DTYPE_PTS:
		sbuf_cat(sb, "/dev/ptmx");
		break;
	default:
unknown:
		sbuf_cat(sb, "anon_inode:[unknown]");
		break;
	}
	fdrop(fp, td);
	return (error);
}

static int
aprocfs_tdpick_drain(void *arg, const char *data, int len)
{
	struct aprocfs_tdpick *tp;
	lwpid_t tid;

	tp = arg;
	KASSERT(len == sizeof(struct kinfo_proc),
	    ("aprocfs: partial kinfo_proc record of %d bytes", len));
	memcpy(&tid, data + offsetof(struct kinfo_proc, ki_tid), sizeof(tid));
	if (!tp->tp_found && tid == tp->tp_tid) {
		memcpy(tp->tp_ki, data, sizeof(*tp->tp_ki));
		tp->tp_found = true;
	}
	return (len);
}

/* Visible pids only: a parent or tracer that the reader cannot see is 0. */
static pid_t
aprocfs_seepid(struct thread *td, pid_t pid)
{
	struct proc *p;
	bool visible;

	if (pid <= 0 || pid > PID_MAX)
		return (0);
	p = pfind(pid);
	if (p == NULL)
		return (0);
	visible = p_cansee(td, p) == 0;
	PROC_UNLOCK(p);
	return (visible ? pid : 0);
}

static int
aprocfs_pinfo_get(struct thread *td, const struct aprocfs_key *key,
    struct aprocfs_pinfo *pi)
{
	struct aprocfs_tdpick tp;
	struct vmspace *vm;
	struct thread *td2;
	struct sbuf rec;
	struct proc *p;
	char *buf;
	int error;

	buf = malloc(sizeof(struct kinfo_proc) + 1, M_APROCFS, M_WAITOK);
	/* fill_kinfo_proc() reads the process group and the session. */
	sx_slock(&proctree_lock);
	error = aprocfs_getproc(td, key, 0, &p);
	if (error != 0) {
		sx_sunlock(&proctree_lock);
		free(buf, M_APROCFS);
		return (error);
	}
	if (key->ak_tid == 0)
		td2 = FIRST_THREAD_IN_PROC(p);
	else {
		FOREACH_THREAD_IN_PROC(p, td2) {
			if (td2->td_tid == key->ak_tid)
				break;
		}
	}
	MPASS(td2 != NULL);
	pi->pi_candebug = p_candebug(td, p) == 0;
	pi->pi_cru = p->p_stats->p_cru;
	pi->pi_shdpnd = p->p_siglist;
	pi->pi_thrpnd = td2->td_siglist;
	thread_lock(td2);
	CPU_COPY(&td2->td_cpuset->cs_mask, &pi->pi_cpus);
	thread_unlock(td2);
	pi->pi_cred = crhold(p->p_ucred);
	/* exit1() frees the limits after it sets P_WEXIT. */
	if ((p->p_flag & P_WEXIT) == 0)
		pi->pi_rsslim = lim_cur_proc(p, RLIMIT_RSS);
	pi->pi_sigparent = p->p_sigparent;
	pi->pi_nonewprivs = (p->p_flag2 & P2_NO_NEW_PRIVS) != 0;
	pi->pi_umask = p->p_pd != NULL ? p->p_pd->pd_cmask : 0;
	vm = p->p_vmspace;
	if (pi->pi_candebug && p->p_state == PRS_NORMAL &&
	    (p->p_flag & (P_SYSTEM | P_WEXIT)) == 0 && vm != NULL) {
		pi->pi_startcode = (vm_offset_t)vm->vm_taddr;
		pi->pi_endcode = pi->pi_startcode + ptoa(vm->vm_tsize);
		pi->pi_startstack = PROC_PS_STRINGS(p);
		pi->pi_startdata = (vm_offset_t)vm->vm_daddr;
		pi->pi_enddata = pi->pi_startdata + ptoa(vm->vm_dsize);
	}

	if (key->ak_tid == 0) {
		fill_kinfo_proc(p, &pi->pi_ki);
		PROC_UNLOCK(p);
	} else {
		/* kern_proc_out() gives one record for each thread. */
		tp.tp_ki = &pi->pi_ki;
		tp.tp_tid = key->ak_tid;
		tp.tp_found = false;
		sbuf_new(&rec, buf, sizeof(struct kinfo_proc) + 1,
		    SBUF_FIXEDLEN);
		sbuf_set_drain(&rec, aprocfs_tdpick_drain, &tp);
		error = kern_proc_out(p, &rec, 0);
		if (error == 0)
			error = sbuf_finish(&rec);
		sbuf_delete(&rec);
		if (error == 0 && !tp.tp_found)
			error = ENOENT;
	}
	sx_sunlock(&proctree_lock);
	free(buf, M_APROCFS);
	if (error != 0) {
		crfree(pi->pi_cred);
		pi->pi_cred = NULL;
		return (error);
	}
	pi->pi_ppid = aprocfs_seepid(td, pi->pi_ki.ki_ppid);
	pi->pi_tracer = aprocfs_seepid(td, pi->pi_ki.ki_tracer);
	return (0);
}

static void
aprocfs_comm(const struct kinfo_proc *ki, char *buf, size_t len)
{

	if (ki->ki_tdname[0] != '\0')
		strlcpy(buf, ki->ki_tdname, len);
	else
		strlcpy(buf, ki->ki_comm, len);
}

static char
aprocfs_state(const struct kinfo_proc *ki)
{

	switch (ki->ki_stat) {
	case SRUN:
	case SIDL:
		return ('R');
	case SSLEEP:
		return ((ki->ki_tdflags & TDF_SINTR) != 0 ? 'S' : 'D');
	case SSTOP:
		return ((ki->ki_flag & P_TRACED) != 0 ? 't' : 'T');
	case SZOMB:
		return ('Z');
	case SLOCK:
		return ('D');
	case SWAIT:
	default:
		return ('S');
	}
}

static const char *
aprocfs_state_name(char state)
{

	switch (state) {
	case 'R':
		return ("running");
	case 'D':
		return ("disk sleep");
	case 'T':
		return ("stopped");
	case 't':
		return ("tracing stop");
	case 'Z':
		return ("zombie");
	case 'S':
	default:
		return ("sleeping");
	}
}

/*
 * The Linux priority field: 20 + nice for a timeshare or idle thread, and
 * -1 - rt_priority for a real-time thread.  rt_priority is the POSIX value
 * that sched_getparam(2) gives, from the user priority, which includes a
 * priority lent through a PI mutex.
 */
static void
aprocfs_sched(const struct kinfo_proc *ki, int *prio, u_int *rtprio,
    u_int *policy)
{
	int level;

	if (PRI_IS_REALTIME(ki->ki_pri.pri_class)) {
		level = ki->ki_pri.pri_user - PRI_MIN_REALTIME;
		if (level < 0)
			level = 0;
		if (level > RTP_PRIO_MAX)
			level = RTP_PRIO_MAX;
		*rtprio = RTP_PRIO_MAX - level;
		*prio = -1 - (int)*rtprio;
		*policy = (ki->ki_pri.pri_class & PRI_FIFO_BIT) != 0 ?
		    SCHED_FIFO : SCHED_RR;
	} else {
		*prio = 20 + ki->ki_nice;
		*rtprio = 0;
		*policy = SCHED_OTHER;
	}
}

static uint64_t
aprocfs_ticks(const struct timeval *tv)
{

	return ((uint64_t)tv->tv_sec * APROCFS_CLK_TCK +
	    (uint64_t)tv->tv_usec / (1000000 / APROCFS_CLK_TCK));
}

/* Signals 1 to 64, for the obsolete decimal fields of stat. */
static uint64_t
aprocfs_sig64(const sigset_t *set)
{

	return ((uint64_t)set->__bits[0] | (uint64_t)set->__bits[1] << 32);
}

/* All 128 signals, as 32 hex digits; bit n - 1 is signal n. */
static void
aprocfs_sigset(struct sbuf *sb, const sigset_t *set)
{
	int i;

	for (i = _SIG_WORDS - 1; i >= 0; i--)
		sbuf_printf(sb, "%08x", set->__bits[i]);
	sbuf_putc(sb, '\n');
}

static int
aprocfs_do_stat(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{
	char comm[APROCFS_COMMLEN];
	struct aprocfs_pinfo *pi;
	struct kinfo_proc *ki;
	struct timeval bt, start;
	u_int policy, rtprio, tdmaj, tdmin, ttynr;
	int error, prio, tpgid, xstat;

	pi = malloc(sizeof(*pi), M_APROCFS, M_WAITOK | M_ZERO);
	error = aprocfs_pinfo_get(td, key, pi);
	if (error != 0)
		goto out;
	ki = &pi->pi_ki;
	aprocfs_comm(ki, comm, sizeof(comm));
	aprocfs_sched(ki, &prio, &rtprio, &policy);
	if (ki->ki_tdev == NODEV) {
		ttynr = 0;
		tpgid = -1;
	} else {
		tdmaj = major(ki->ki_tdev);
		tdmin = minor(ki->ki_tdev);
		ttynr = (tdmin & 0xff) | ((tdmaj & 0xfff) << 8) |
		    ((tdmin & ~0xffU) << 12);
		tpgid = ki->ki_tpgid == NO_PID ? -1 : ki->ki_tpgid;
	}
	getboottime(&bt);
	start = ki->ki_start;
	timevalsub(&start, &bt);
	if (start.tv_sec < 0)
		timevalclear(&start);
	xstat = pi->pi_candebug && ki->ki_stat == SZOMB ? ki->ki_xstat : 0;

	/* Fields 1 to 13: pid (comm) state ppid pgrp session ... cmajflt. */
	sbuf_printf(sb, "%d (%s) %c %d %d %d %u %d 0 %ju %ju %ju %ju ",
	    key->ak_tid != 0 ? ki->ki_tid : ki->ki_pid, comm,
	    aprocfs_state(ki), pi->pi_ppid, ki->ki_pgid, ki->ki_sid, ttynr,
	    tpgid, (uintmax_t)ki->ki_rusage.ru_minflt,
	    (uintmax_t)pi->pi_cru.ru_minflt,
	    (uintmax_t)ki->ki_rusage.ru_majflt,
	    (uintmax_t)pi->pi_cru.ru_majflt);
	/* Fields 14 to 25: utime ... rsslim. */
	sbuf_printf(sb, "%ju %ju %ju %ju %d %d %d 0 %ju %ju %jd %ju ",
	    (uintmax_t)aprocfs_ticks(&ki->ki_rusage.ru_utime),
	    (uintmax_t)aprocfs_ticks(&ki->ki_rusage.ru_stime),
	    (uintmax_t)aprocfs_ticks(&ki->ki_childutime),
	    (uintmax_t)aprocfs_ticks(&ki->ki_childstime),
	    prio, ki->ki_nice, ki->ki_numthreads,
	    (uintmax_t)aprocfs_ticks(&start), (uintmax_t)ki->ki_size,
	    (intmax_t)ki->ki_rssize, (uintmax_t)pi->pi_rsslim);
	/* Fields 26 to 37: startcode ... cnswap. */
	sbuf_printf(sb, "%ju %ju %ju 0 0 %ju %ju %ju %ju 0 0 0 ",
	    (uintmax_t)pi->pi_startcode, (uintmax_t)pi->pi_endcode,
	    (uintmax_t)pi->pi_startstack,
	    (uintmax_t)aprocfs_sig64(&ki->ki_siglist),
	    (uintmax_t)aprocfs_sig64(&ki->ki_sigmask),
	    (uintmax_t)aprocfs_sig64(&ki->ki_sigignore),
	    (uintmax_t)aprocfs_sig64(&ki->ki_sigcatch));
	/* Fields 38 to 52: exit_signal ... exit_code. */
	sbuf_printf(sb, "%d %d %u %u 0 0 0 %ju %ju 0 0 0 0 0 %d\n",
	    pi->pi_sigparent, ki->ki_lastcpu == NOCPU ? 0 : ki->ki_lastcpu,
	    rtprio, policy, (uintmax_t)pi->pi_startdata,
	    (uintmax_t)pi->pi_enddata, xstat);
	crfree(pi->pi_cred);
out:
	free(pi, M_APROCFS);
	return (error);
}

static int
aprocfs_do_status(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb)
{
	char comm[APROCFS_COMMLEN];
	struct aprocfs_pinfo *pi;
	struct kinfo_proc *ki;
	struct ucred *cr;
	pid_t pid;
	int error, i;
	char state;

	pi = malloc(sizeof(*pi), M_APROCFS, M_WAITOK | M_ZERO);
	error = aprocfs_pinfo_get(td, key, pi);
	if (error != 0)
		goto out;
	ki = &pi->pi_ki;
	cr = pi->pi_cred;
	aprocfs_comm(ki, comm, sizeof(comm));
	state = aprocfs_state(ki);
	pid = key->ak_tid != 0 ? ki->ki_tid : ki->ki_pid;

	sbuf_printf(sb, "Name:\t%s\n", comm);
	sbuf_printf(sb, "Umask:\t%04o\n", pi->pi_umask);
	sbuf_printf(sb, "State:\t%c (%s)\n", state, aprocfs_state_name(state));
	sbuf_printf(sb, "Tgid:\t%d\n", ki->ki_pid);
	sbuf_printf(sb, "Ngid:\t0\n");
	sbuf_printf(sb, "Pid:\t%d\n", pid);
	sbuf_printf(sb, "PPid:\t%d\n", pi->pi_ppid);
	sbuf_printf(sb, "TracerPid:\t%d\n", pi->pi_tracer);
	sbuf_printf(sb, "Uid:\t%u\t%u\t%u\t%u\n", cr->cr_ruid, cr->cr_uid,
	    cr->cr_svuid, cr->cr_uid);
	sbuf_printf(sb, "Gid:\t%u\t%u\t%u\t%u\n", cr->cr_rgid, cr->cr_gid,
	    cr->cr_svgid, cr->cr_gid);
	sbuf_printf(sb, "Groups:\t");
	for (i = 0; i < cr->cr_ngroups; i++)
		sbuf_printf(sb, i == 0 ? "%u" : " %u", cr->cr_groups[i]);
	sbuf_putc(sb, '\n');
	sbuf_printf(sb, "NStgid:\t%d\n", ki->ki_pid);
	sbuf_printf(sb, "NSpid:\t%d\n", pid);
	sbuf_printf(sb, "NSpgid:\t%d\n", ki->ki_pgid);
	sbuf_printf(sb, "NSsid:\t%d\n", ki->ki_sid);
	sbuf_printf(sb, "VmSize:\t%8ju kB\n", (uintmax_t)ki->ki_size / 1024);
	sbuf_printf(sb, "VmHWM:\t%8ju kB\n",
	    (uintmax_t)ki->ki_rusage.ru_maxrss);
	sbuf_printf(sb, "VmRSS:\t%8ju kB\n",
	    (uintmax_t)ptoa(ki->ki_rssize) / 1024);
	sbuf_printf(sb, "VmData:\t%8ju kB\n",
	    (uintmax_t)ptoa(ki->ki_dsize) / 1024);
	sbuf_printf(sb, "VmStk:\t%8ju kB\n",
	    (uintmax_t)ptoa(ki->ki_ssize) / 1024);
	sbuf_printf(sb, "VmExe:\t%8ju kB\n",
	    (uintmax_t)ptoa(ki->ki_tsize) / 1024);
	sbuf_printf(sb, "Threads:\t%d\n", ki->ki_numthreads);
	sbuf_printf(sb, "SigPnd:\t");
	aprocfs_sigset(sb, &pi->pi_thrpnd);
	sbuf_printf(sb, "ShdPnd:\t");
	aprocfs_sigset(sb, &pi->pi_shdpnd);
	sbuf_printf(sb, "SigBlk:\t");
	aprocfs_sigset(sb, &ki->ki_sigmask);
	sbuf_printf(sb, "SigIgn:\t");
	aprocfs_sigset(sb, &ki->ki_sigignore);
	sbuf_printf(sb, "SigCgt:\t");
	aprocfs_sigset(sb, &ki->ki_sigcatch);
	/* FreeBSD has no capability sets. */
	sbuf_printf(sb, "CapInh:\t%016x\nCapPrm:\t%016x\nCapEff:\t%016x\n"
	    "CapBnd:\t%016x\nCapAmb:\t%016x\n", 0, 0, 0, 0, 0);
	sbuf_printf(sb, "NoNewPrivs:\t%d\n", pi->pi_nonewprivs ? 1 : 0);
	sbuf_printf(sb, "Seccomp:\t0\n");
	sbuf_printf(sb, "Cpus_allowed:\t");
	aprocfs_cpumask(sb, &pi->pi_cpus);
	sbuf_printf(sb, "Cpus_allowed_list:\t");
	aprocfs_cpulist(sb, &pi->pi_cpus);
	sbuf_printf(sb, "voluntary_ctxt_switches:\t%ld\n",
	    ki->ki_rusage.ru_nvcsw);
	sbuf_printf(sb, "nonvoluntary_ctxt_switches:\t%ld\n",
	    ki->ki_rusage.ru_nivcsw);
	crfree(pi->pi_cred);
out:
	free(pi, M_APROCFS);
	return (error);
}

/* The hex mask of the CPUs up to mp_maxid, in words of 32 bits. */
static void
aprocfs_cpumask(struct sbuf *sb, const cpuset_t *set)
{
	int b, d, ndig, nib;

	ndig = (mp_maxid + 4) / 4;
	for (d = ndig - 1; d >= 0; d--) {
		nib = 0;
		for (b = 0; b < 4; b++) {
			if (d * 4 + b <= (int)mp_maxid &&
			    CPU_ISSET(d * 4 + b, set))
				nib |= 1 << b;
		}
		sbuf_putc(sb, hex2ascii(nib));
		if (d != 0 && d % 8 == 0)
			sbuf_putc(sb, ',');
	}
	sbuf_putc(sb, '\n');
}

/* The list format of cpuset(7), for example "0-3,6". */
static void
aprocfs_cpulist(struct sbuf *sb, const cpuset_t *set)
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

static void
aprocfs_maps_line(struct aprocfs_mapsctx *mc,
    const struct kinfo_vmentry *kve)
{
	struct sbuf *sb;
	const char *name, *s;
	uintmax_t ino, offset;
	ssize_t start;
	u_int devmaj, devmin;
	bool cow, shared;

	sb = mc->mc_out;
	name = "";
	cow = (kve->kve_flags & KVME_FLAG_COW) != 0;
	shared = false;
	offset = 0;
	ino = 0;
	devmaj = devmin = 0;
	switch (kve->kve_type) {
	case KVME_TYPE_VNODE:
		name = kve->kve_path;
		shared = !cow;
		offset = kve->kve_offset;
		if ((kve->kve_status & KF_ATTR_VALID) != 0) {
			ino = kve->kve_vn_fileid;
			devmaj = major(kve->kve_vn_fsid);
			devmin = minor(kve->kve_vn_fsid);
		}
		break;
	case KVME_TYPE_DEVICE:
	case KVME_TYPE_MGTDEVICE:
	case KVME_TYPE_SG:
		name = kve->kve_path;
		shared = !cow;
		offset = kve->kve_offset;
		break;
	case KVME_TYPE_PHYS:
		if (mc->mc_shp != 0 && kve->kve_start == mc->mc_shp)
			name = "[vdso]";
		break;
	case KVME_TYPE_SWAP:
	case KVME_TYPE_DEFAULT:
		if ((kve->kve_flags &
		    (KVME_FLAG_POSIXSHM | KVME_FLAG_SYSVSHM)) != 0) {
			name = kve->kve_path;
			shared = !cow;
			offset = kve->kve_offset;
		}
		break;
	default:
		break;
	}
	if ((kve->kve_flags & KVME_FLAG_GROWS_DOWN) != 0)
		name = "[stack]";

	start = sbuf_len(sb);
	sbuf_printf(sb, "%08jx-%08jx %c%c%c%c %08jx %02x:%02x %ju",
	    (uintmax_t)kve->kve_start, (uintmax_t)kve->kve_end,
	    (kve->kve_protection & KVME_PROT_READ) != 0 ? 'r' : '-',
	    (kve->kve_protection & KVME_PROT_WRITE) != 0 ? 'w' : '-',
	    (kve->kve_protection & KVME_PROT_EXEC) != 0 ? 'x' : '-',
	    shared ? 's' : 'p', offset, devmaj, devmin, ino);
	if (name[0] != '\0') {
		while (sbuf_len(sb) - start < APROCFS_MAPS_NAMECOL)
			sbuf_putc(sb, ' ');
		sbuf_putc(sb, ' ');
		/* Linux shows a newline in a path as \012. */
		for (s = name; *s != '\0'; s++) {
			if (*s == '\n')
				sbuf_cat(sb, "\\012");
			else
				sbuf_putc(sb, *s);
		}
	}
	sbuf_putc(sb, '\n');
}

/*
 * kern_proc_vmmap_out() writes one kinfo_vmentry for each map entry, with
 * the map unlocked, into a buffer of one record.  Each drain is one record;
 * turn it into a line.  Stop after the part that the read asked for.
 */
static int
aprocfs_maps_drain(void *arg, const char *data, int len)
{
	struct aprocfs_mapsctx *mc;

	mc = arg;
	KASSERT(len == sizeof(struct kinfo_vmentry),
	    ("aprocfs: partial kinfo_vmentry record of %d bytes", len));
	if (sbuf_len(mc->mc_out) > mc->mc_limit) {
		mc->mc_stopped = true;
		return (-EFBIG);
	}
	aprocfs_maps_line(mc, (const struct kinfo_vmentry *)(const void *)data);
	return (len);
}

static int
aprocfs_do_maps(struct thread *td, const struct aprocfs_key *key,
    struct sbuf *sb, off_t limit)
{
	struct aprocfs_mapsctx mc;
	struct sbuf rec;
	struct proc *p;
	char *buf;
	int error, error2;

	buf = malloc(sizeof(struct kinfo_vmentry) + 1, M_APROCFS, M_WAITOK);
	error = aprocfs_getproc(td, key, PGET_CANDEBUG, &p);
	if (error != 0) {
		free(buf, M_APROCFS);
		return (error);
	}
	if (p->p_state != PRS_NORMAL ||
	    (p->p_flag & (P_WEXIT | P_SYSTEM)) != 0) {
		PROC_UNLOCK(p);
		free(buf, M_APROCFS);
		return (0);
	}
	mc.mc_out = sb;
	mc.mc_limit = limit;
	mc.mc_stopped = false;
	mc.mc_shp = (p->p_sysent->sv_flags & SV_SHP) != 0 ?
	    p->p_vmspace->vm_shp_base : 0;
	sbuf_new(&rec, buf, sizeof(struct kinfo_vmentry) + 1, SBUF_FIXEDLEN);
	sbuf_set_drain(&rec, aprocfs_maps_drain, &mc);
	error = kern_proc_vmmap_out(p, &rec, -1, 0);
	error2 = sbuf_finish(&rec);
	sbuf_delete(&rec);
	free(buf, M_APROCFS);
	if (mc.mc_stopped)
		return (0);
	return (error != 0 ? error : error2);
}
