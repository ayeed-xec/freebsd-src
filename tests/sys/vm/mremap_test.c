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
 * Tests of mremap(2).  Each error of the manual page has a case.  The other
 * cases shrink, grow, move and map again private and shared anonymous
 * memory, files, POSIX shared memory, large pages and wired memory, and
 * check the write counts, the resource limits and the map after fork(2).
 */

#include <sys/param.h>
#include <sys/capsicum.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atf-c.h>

#define	MAYMOVE		MREMAP_MAYMOVE
#define	FIXED		(MREMAP_MAYMOVE | MREMAP_FIXED)
#define	DONTUNMAP	(MREMAP_MAYMOVE | MREMAP_DONTUNMAP)

static size_t
pagesize(void)
{
	return ((size_t)getpagesize());
}

static char *
map_anon(size_t len, int prot, int flags)
{
	void *p;

	p = mmap(NULL, len, prot, MAP_ANON | flags, -1, 0);
	ATF_REQUIRE_MSG(p != MAP_FAILED, "mmap: %s", strerror(errno));
	return (p);
}

/* Map "len" bytes of private memory with "room" free bytes after them. */
static char *
map_with_room(size_t len, size_t room)
{
	char *p;

	p = map_anon(len + room, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	ATF_REQUIRE(munmap(p + len, room) == 0);
	return (p);
}

/* Fill each page of [p, p + len) with its own byte, from "seed" up. */
static void
fill(char *p, size_t len, int seed)
{
	size_t off;

	for (off = 0; off < len; off += pagesize())
		memset(p + off, (seed + (int)(off / pagesize())) & 0xff,
		    pagesize());
}

static bool
check(const char *p, size_t len, int seed)
{
	size_t off;
	char c;

	for (off = 0; off < len; off += pagesize()) {
		c = (char)((seed + (int)(off / pagesize())) & 0xff);
		if (p[off] != c || p[off + pagesize() - 1] != c)
			return (false);
	}
	return (true);
}

static bool
zero(const char *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0)
			return (false);
	}
	return (true);
}

/* Return true if no page of [p, p + len) is mapped. */
static bool
unmapped(const char *p, size_t len)
{
	size_t off;
	char vec;

	for (off = 0; off < len; off += pagesize()) {
		if (mincore(p + off, pagesize(), &vec) == 0 || errno != ENOMEM)
			return (false);
	}
	return (true);
}

/*
 * Return the number of map entries in [p, p + len), and a copy of the
 * first one in "*first".
 */
static int
entries(const char *p, size_t len, struct kinfo_vmentry *first)
{
	struct kinfo_vmentry *kve;
	uint64_t end, start;
	int cnt, i, n;

	kve = kinfo_getvmmap(getpid(), &cnt);
	ATF_REQUIRE(kve != NULL);
	start = (uintptr_t)p;
	end = start + len;
	n = 0;
	for (i = 0; i < cnt; i++) {
		if (kve[i].kve_end <= start || kve[i].kve_start >= end)
			continue;
		if (n++ == 0 && first != NULL)
			*first = kve[i];
	}
	free(kve);
	return (n);
}

/* Write to "p" in a child and return the signal that killed it, or 0. */
static int
touch_signal(volatile char *p)
{
	struct rlimit rl = { 0, 0 };
	pid_t pid;
	int status;

	pid = fork();
	ATF_REQUIRE(pid != -1);
	if (pid == 0) {
		(void)setrlimit(RLIMIT_CORE, &rl);
		*p = 1;
		_exit(0);
	}
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	return (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* Make a file of "len" bytes that each hold "c". */
static int
make_file(const char *path, size_t len, char c)
{
	char *buf;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	ATF_REQUIRE_MSG(fd != -1, "open %s: %s", path, strerror(errno));
	buf = malloc(len);
	ATF_REQUIRE(buf != NULL);
	memset(buf, c, len);
	ATF_REQUIRE(write(fd, buf, len) == (ssize_t)len);
	free(buf);
	return (fd);
}

static char
file_byte(int fd, off_t off)
{
	char c;

	ATF_REQUIRE(pread(fd, &c, 1, off) == 1);
	return (c);
}

ATF_TC_WITHOUT_HEAD(einval);
ATF_TC_BODY(einval, tc)
{
	char *p, *q;
	size_t pg;
	int fd;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	q = map_anon(pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 2 * pg, 1);

	/* Flags. */
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, pg, 0x8) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(p, pg, pg, MREMAP_FIXED, q) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(p, pg, pg, MREMAP_DONTUNMAP) == MAP_FAILED);

	/* The old address and the new size. */
	ATF_CHECK_ERRNO(EINVAL, mremap(p + 1, pg, pg, 0) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, 0, MAYMOVE) == MAP_FAILED);

	/* The new address: not aligned, outside the map, over the old range. */
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, pg, FIXED, q + 1) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, pg, FIXED,
	    (void *)-(uintptr_t)(2 * pg)) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(p, 2 * pg, pg, FIXED, p + pg) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p + pg, pg, 2 * pg, FIXED, p) ==
	    MAP_FAILED);

	/* MREMAP_DONTUNMAP changes no size. */
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(p, pg, 2 * pg, DONTUNMAP) == MAP_FAILED);

	/* An old size of 0 needs MREMAP_MAYMOVE and a shared mapping. */
	ATF_CHECK_ERRNO(EINVAL, mremap(p, 0, pg, 0) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, 0, pg, MAYMOVE) == MAP_FAILED);

	/* MREMAP_DONTUNMAP of a private file mapping. */
	fd = make_file("file", 2 * pg, 'a');
	q = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ATF_REQUIRE(q != MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(q, 2 * pg, 2 * pg, DONTUNMAP) == MAP_FAILED);
	ATF_CHECK(q[0] == 'a');

	/* Nothing changed. */
	ATF_CHECK(check(p, 2 * pg, 1));
	ATF_CHECK_EQ(1, entries(p, 2 * pg, NULL));
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(einval_entries);
ATF_TC_BODY(einval_entries, tc)
{
	char *p, *text;
	size_t pg;
	int id;

	pg = pagesize();

	/* A guard. */
	p = mmap(NULL, 2 * pg, PROT_NONE, MAP_GUARD, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, 4 * pg, MAYMOVE) == MAP_FAILED);

	/* A stack, with its gap at the bottom. */
	p = mmap(NULL, 8 * pg, PROT_READ | PROT_WRITE, MAP_STACK, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(p + 7 * pg, pg, 2 * pg, MAYMOVE) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, 2 * pg, MAYMOVE) == MAP_FAILED);

	/* Executable memory, and the text of this program. */
	p = map_anon(pg, PROT_READ | PROT_EXEC, MAP_PRIVATE);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, 2 * pg, MAYMOVE) == MAP_FAILED);
	text = (char *)trunc_page((uintptr_t)&pagesize);
	ATF_CHECK_ERRNO(EINVAL, mremap(text, pg, pg, 0) == MAP_FAILED);

	/* SysV shared memory: shmdt(2) finds the segment by its address. */
	id = shmget(IPC_PRIVATE, pg, IPC_CREAT | 0600);
	ATF_REQUIRE(id != -1);
	p = shmat(id, NULL, 0);
	ATF_REQUIRE(p != (void *)-1);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, pg, pg, 0) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, 0, pg, MAYMOVE) == MAP_FAILED);
	ATF_CHECK(shmdt(p) == 0);
	ATF_CHECK(shmctl(id, IPC_RMID, NULL) == 0);
}

ATF_TC_WITHOUT_HEAD(efault);
ATF_TC_BODY(efault, tc)
{
	char *p;
	size_t pg;

	pg = pagesize();
	p = map_anon(4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);

	/* Not mapped, a hole, and an old size that overflows. */
	ATF_REQUIRE(munmap(p + 2 * pg, pg) == 0);
	ATF_CHECK_ERRNO(EFAULT, mremap(p + 2 * pg, pg, pg, 0) == MAP_FAILED);
	ATF_CHECK_ERRNO(EFAULT,
	    mremap(p, 4 * pg, 4 * pg, MAYMOVE) == MAP_FAILED);
	ATF_CHECK_ERRNO(EFAULT, mremap(p, SIZE_MAX, pg, 0) == MAP_FAILED);

	/* Entries of different kinds. */
	ATF_REQUIRE(mprotect(p + pg, pg, PROT_READ) == 0);
	ATF_CHECK_ERRNO(EFAULT,
	    mremap(p, 2 * pg, 3 * pg, MAYMOVE) == MAP_FAILED);
}

ATF_TC_WITHOUT_HEAD(enomem);
ATF_TC_BODY(enomem, tc)
{
	char *p;
	size_t pg;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 2 * pg, 2);

	/* No room to grow, and a new size that overflows. */
	ATF_CHECK_ERRNO(ENOMEM, mremap(p, pg, 2 * pg, 0) == MAP_FAILED);
	ATF_CHECK_ERRNO(ENOMEM,
	    mremap(p, pg, SIZE_MAX, MAYMOVE) == MAP_FAILED);
	ATF_CHECK_ERRNO(ENOMEM,
	    mremap(p, pg, SIZE_MAX - 2 * pg, MAYMOVE) == MAP_FAILED);
	ATF_CHECK(check(p, 2 * pg, 2));
	ATF_CHECK_EQ(1, entries(p, 2 * pg, NULL));
}

ATF_TC_WITHOUT_HEAD(same_size);
ATF_TC_BODY(same_size, tc)
{
	char *p;
	size_t pg;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 2 * pg, 3);
	ATF_CHECK(mremap(p, 2 * pg, 2 * pg, 0) == p);
	ATF_CHECK(mremap(p, 2 * pg, 2 * pg, MAYMOVE) == p);
	ATF_CHECK(mremap(p, pg + 1, 2 * pg - 1, 0) == p);
	ATF_CHECK(check(p, 2 * pg, 3));
}

ATF_TC_WITHOUT_HEAD(shrink);
ATF_TC_BODY(shrink, tc)
{
	char *p;
	size_t pg;

	pg = pagesize();
	p = map_anon(4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 4 * pg, 4);
	ATF_REQUIRE(mremap(p, 4 * pg, pg, 0) == p);
	ATF_CHECK(check(p, pg, 4));
	ATF_CHECK(unmapped(p + pg, 3 * pg));
}

ATF_TC_WITHOUT_HEAD(grow_in_place);
ATF_TC_BODY(grow_in_place, tc)
{
	char *p;
	size_t pg;

	pg = pagesize();
	p = map_with_room(pg, 3 * pg);
	fill(p, pg, 5);
	ATF_REQUIRE(mremap(p, pg, 4 * pg, 0) == p);
	ATF_CHECK(check(p, pg, 5));
	ATF_CHECK(zero(p + pg, 3 * pg));
	fill(p, 4 * pg, 6);
	ATF_CHECK(check(p, 4 * pg, 6));

	/* The object grew: one entry. */
	ATF_CHECK_EQ(1, entries(p, 4 * pg, NULL));
}

ATF_TC_WITHOUT_HEAD(grow_move);
ATF_TC_BODY(grow_move, tc)
{
	char *p, *q;
	size_t pg;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 2 * pg, 7);
	q = mremap(p, pg, 3 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(q != p);
	ATF_CHECK(check(q, pg, 7));
	ATF_CHECK(zero(q + pg, 2 * pg));
	fill(q, 3 * pg, 9);
	ATF_CHECK(check(q, 3 * pg, 9));
	ATF_CHECK(unmapped(p, pg));
	ATF_CHECK(check(p + pg, pg, 8));
}

ATF_TC_WITHOUT_HEAD(move_protection);
ATF_TC_BODY(move_protection, tc)
{
	struct kinfo_vmentry kve;
	char *p, *q;
	size_t pg;

	pg = pagesize();
	p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE |
	    PROT_MAX(PROT_READ | PROT_WRITE), MAP_ANON | MAP_PRIVATE, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	fill(p, 2 * pg, 10);
	ATF_REQUIRE(mprotect(p, 2 * pg, PROT_READ) == 0);
	q = map_anon(2 * pg, PROT_NONE, MAP_PRIVATE);
	ATF_REQUIRE(mremap(p, 2 * pg, 2 * pg, FIXED, q) == q);
	ATF_CHECK(check(q, 2 * pg, 10));
	ATF_CHECK(unmapped(p, 2 * pg));
	ATF_REQUIRE_EQ(1, entries(q, 2 * pg, &kve));
	ATF_CHECK_EQ(KVME_PROT_READ | KVME_MAX_PROT_READ | KVME_MAX_PROT_WRITE,
	    kve.kve_protection);

	/* The maximum protection moved too. */
	ATF_CHECK_ERRNO(EACCES, mprotect(q, 2 * pg, PROT_EXEC) == -1);
	ATF_CHECK(mprotect(q, 2 * pg, PROT_READ | PROT_WRITE) == 0);
	q[0] = 1;
}

ATF_TC_WITHOUT_HEAD(fixed);
ATF_TC_BODY(fixed, tc)
{
	char *p, *q;
	size_t pg;

	pg = pagesize();
	q = map_anon(4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(q, 4 * pg, 20);

	/* Replace the middle of a mapping. */
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 2 * pg, 30);
	ATF_REQUIRE(mremap(p, 2 * pg, 2 * pg, FIXED, q + pg) == q + pg);
	ATF_CHECK(check(q, pg, 20));
	ATF_CHECK(check(q + pg, 2 * pg, 30));
	ATF_CHECK(check(q + 3 * pg, pg, 23));
	ATF_CHECK(unmapped(p, 2 * pg));

	/* Grow. */
	p = map_anon(pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, pg, 40);
	ATF_REQUIRE(mremap(p, pg, 3 * pg, FIXED, q) == q);
	ATF_CHECK(check(q, pg, 40));
	ATF_CHECK(zero(q + pg, 2 * pg));
	ATF_CHECK(check(q + 3 * pg, pg, 23));
	ATF_CHECK(unmapped(p, pg));

	/* Shrink. */
	p = map_anon(3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 3 * pg, 50);
	ATF_REQUIRE(mremap(p, 3 * pg, pg, FIXED, q + 3 * pg) == q + 3 * pg);
	ATF_CHECK(check(q + 3 * pg, pg, 50));
	ATF_CHECK(unmapped(p, 3 * pg));

	/* Next to the old range, below and above it. */
	p = map_with_room(2 * pg, 2 * pg);
	fill(p, 2 * pg, 60);
	ATF_REQUIRE(mremap(p, 2 * pg, 2 * pg, FIXED, p + 2 * pg) == p + 2 * pg);
	ATF_CHECK(check(p + 2 * pg, 2 * pg, 60));
	ATF_CHECK(unmapped(p, 2 * pg));
	ATF_REQUIRE(mremap(p + 2 * pg, pg, 2 * pg, FIXED, p) == p);
	ATF_CHECK(check(p, pg, 60));
	ATF_CHECK(zero(p + pg, pg));
	ATF_CHECK(check(p + 3 * pg, pg, 61));

	/*
	 * An untouched entry has no object yet, so the new tail below it
	 * merges with it before the move.
	 */
	p = map_anon(3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	ATF_REQUIRE(munmap(p, 3 * pg) == 0);
	ATF_REQUIRE(mmap(p + 2 * pg, pg, PROT_READ | PROT_WRITE, MAP_ANON |
	    MAP_PRIVATE | MAP_FIXED | MAP_EXCL, -1, 0) == p + 2 * pg);
	ATF_REQUIRE(mremap(p + 2 * pg, pg, 2 * pg, FIXED, p) == p);
	ATF_CHECK(zero(p, 2 * pg));
	ATF_CHECK(unmapped(p + 2 * pg, pg));
	fill(p, 2 * pg, 65);
	ATF_CHECK(check(p, 2 * pg, 65));
}

ATF_TC_WITHOUT_HEAD(dontunmap_private);
ATF_TC_BODY(dontunmap_private, tc)
{
	struct kinfo_vmentry kve, kve_old;
	char *p, *q;
	size_t pg;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 2 * pg, 70);
	ATF_REQUIRE_EQ(1, entries(p, 2 * pg, &kve_old));
	q = mremap(p, 2 * pg, 2 * pg, DONTUNMAP);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(q != p);
	ATF_CHECK(check(q, 2 * pg, 70));
	ATF_CHECK(zero(p, 2 * pg));
	ATF_REQUIRE_EQ(1, entries(p, 2 * pg, &kve));
	ATF_CHECK_EQ(kve_old.kve_protection, kve.kve_protection);
	fill(p, 2 * pg, 80);
	ATF_CHECK(check(q, 2 * pg, 70));

	/* To a fixed address. */
	p = map_anon(pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, pg, 90);
	ATF_REQUIRE(mremap(p, pg, pg, DONTUNMAP | MREMAP_FIXED, q) == q);
	ATF_CHECK(check(q, pg, 90));
	ATF_CHECK(check(q + pg, pg, 71));
	ATF_CHECK(zero(p, pg));
}

ATF_TC_WITHOUT_HEAD(dontunmap_shared);
ATF_TC_BODY(dontunmap_shared, tc)
{
	char *p, *q;
	size_t pg;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED);
	fill(p, 2 * pg, 100);
	q = mremap(p, 2 * pg, 2 * pg, DONTUNMAP);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(check(q, 2 * pg, 100));
	ATF_CHECK(check(p, 2 * pg, 100));
	q[0] = 7;
	ATF_CHECK(p[0] == 7);
	ATF_REQUIRE(munmap(q, 2 * pg) == 0);
	ATF_CHECK(p[0] == 7);
}

ATF_TC_WITHOUT_HEAD(copy_shared_anon);
ATF_TC_BODY(copy_shared_anon, tc)
{
	char *p, *q;
	size_t pg;

	pg = pagesize();
	p = map_anon(2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED);
	fill(p, 2 * pg, 110);
	q = mremap(p, 0, 2 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(check(q, 2 * pg, 110));
	q[0] = 3;
	ATF_CHECK(p[0] == 3);
	ATF_REQUIRE(munmap(p, 2 * pg) == 0);
	ATF_CHECK(q[0] == 3);
	ATF_CHECK(check(q + pg, pg, 111));

	/* Shared memory that no fault has touched yet. */
	p = map_anon(pg, PROT_READ | PROT_WRITE, MAP_SHARED);
	q = mremap(p, 0, pg, MAYMOVE);
	ATF_REQUIRE(q != MAP_FAILED);
	q[0] = 5;
	ATF_CHECK(p[0] == 5);

	/* Over the rest of the old mapping: its pages stay in the object. */
	p = map_anon(4 * pg, PROT_READ | PROT_WRITE, MAP_SHARED);
	fill(p, 4 * pg, 115);
	ATF_REQUIRE(mremap(p, 0, 4 * pg, FIXED, p + pg) == p + pg);
	ATF_CHECK(check(p, pg, 115));
	ATF_CHECK(check(p + pg, 4 * pg, 115));
}

ATF_TC_WITHOUT_HEAD(copy_shm);
ATF_TC_BODY(copy_shm, tc)
{
	char *p, *q, *r;
	size_t pg;
	int fd;

	pg = pagesize();
	fd = shm_open(SHM_ANON, O_RDWR, 0600);
	ATF_REQUIRE(fd != -1);
	ATF_REQUIRE(ftruncate(fd, 3 * pg) == 0);
	p = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	fill(p, 3 * pg, 120);

	/* A second mapping from the second page of the object. */
	q = mremap(p + pg, 0, 2 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(check(q, 2 * pg, 121));
	ATF_REQUIRE(munmap(p, 3 * pg) == 0);
	q[0] = 9;
	ATF_CHECK(check(q + pg, pg, 122));

	/* Past the end of the object a fault gives SIGBUS. */
	r = mremap(q, 0, 4 * pg, MAYMOVE);
	ATF_REQUIRE(r != MAP_FAILED);
	ATF_CHECK(r[0] == 9);
	ATF_CHECK_EQ(0, touch_signal(r + pg));
	ATF_CHECK_EQ(SIGBUS, touch_signal(r + 2 * pg));
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(file_shared);
ATF_TC_BODY(file_shared, tc)
{
	char *p, *q;
	size_t pg;
	int fd;

	pg = pagesize();
	fd = make_file("file", pg, 'a');
	p = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	q = mremap(p, pg, 2 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(q[0] == 'a');
	q[0] = 'b';
	ATF_CHECK(file_byte(fd, 0) == 'b');

	/* The new tail maps the file past its end, then the new end. */
	ATF_CHECK_EQ(SIGBUS, touch_signal(q + pg));
	ATF_REQUIRE(ftruncate(fd, 2 * pg) == 0);
	q[pg] = 'c';
	ATF_CHECK(file_byte(fd, pg) == 'c');
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(file_private);
ATF_TC_BODY(file_private, tc)
{
	char *p, *q, *r;
	size_t pg;
	int fd;

	pg = pagesize();
	fd = make_file("file", 3 * pg, 'a');

	/* A move keeps the private copy, and the file does not change. */
	p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	p[0] = 'b';
	q = map_anon(2 * pg, PROT_NONE, MAP_PRIVATE);
	ATF_REQUIRE(mremap(p, 2 * pg, 2 * pg, FIXED, q) == q);
	ATF_CHECK(q[0] == 'b');
	ATF_CHECK(q[pg] == 'a');
	q[pg] = 'c';
	ATF_CHECK(file_byte(fd, 0) == 'a');
	ATF_CHECK(file_byte(fd, pg) == 'a');

	/* The tail of a written private file mapping is not supported. */
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(q, 2 * pg, 3 * pg, MAYMOVE) == MAP_FAILED);

	/* An unwritten one grows with the next page of the file. */
	r = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ATF_REQUIRE(r != MAP_FAILED);
	r = mremap(r, pg, 3 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(r != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(r[2 * pg] == 'a');
	r[2 * pg] = 'd';
	ATF_CHECK(file_byte(fd, 2 * pg) == 'a');
	ATF_REQUIRE(close(fd) == 0);
}

/* Run "path" in a child and return the errno of execve(2), or 0. */
static int
exec_errno(const char *path)
{
	char *const argv[] = { __DECONST(char *, path), NULL };
	char *const envp[] = { NULL };
	pid_t pid;
	int status;

	pid = fork();
	ATF_REQUIRE(pid != -1);
	if (pid == 0) {
		execve(path, argv, envp);
		_exit(errno);
	}
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	ATF_REQUIRE(WIFEXITED(status));
	return (WEXITSTATUS(status));
}

ATF_TC_WITHOUT_HEAD(writecount_text);
ATF_TC_BODY(writecount_text, tc)
{
	struct stat st;
	char *buf, *p, *q, path[PATH_MAX];
	size_t len;
	int fd, src;

	/* A copy of true(1) that a shared writable mapping maps. */
	ATF_REQUIRE(getcwd(path, sizeof(path) - 8) != NULL);
	strlcat(path, "/true", sizeof(path));
	src = open("/usr/bin/true", O_RDONLY);
	ATF_REQUIRE(src != -1);
	ATF_REQUIRE(fstat(src, &st) == 0);
	len = st.st_size;
	buf = malloc(len);
	ATF_REQUIRE(buf != NULL);
	ATF_REQUIRE(read(src, buf, len) == (ssize_t)len);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0755);
	ATF_REQUIRE(fd != -1);
	ATF_REQUIRE(write(fd, buf, len) == (ssize_t)len);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_REQUIRE(close(fd) == 0);
	ATF_REQUIRE(close(src) == 0);
	len = round_page(len);
	ATF_CHECK_EQ(ETXTBSY, exec_errno(path));

	/* A move, a second mapping and a shrink keep the write count. */
	q = mremap(p, len, len, MAYMOVE | MREMAP_DONTUNMAP);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_REQUIRE(munmap(p, len) == 0);
	ATF_CHECK_EQ(ETXTBSY, exec_errno(path));
	p = mremap(q, 0, len, MAYMOVE);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_REQUIRE(munmap(q, len) == 0);
	ATF_CHECK_EQ(ETXTBSY, exec_errno(path));
	if (len > pagesize()) {
		ATF_REQUIRE(mremap(p, len, pagesize(), 0) == p);
		len = pagesize();
	}
	ATF_CHECK_EQ(ETXTBSY, exec_errno(path));
	ATF_REQUIRE(munmap(p, len) == 0);
	ATF_CHECK_EQ(0, exec_errno(path));
	free(buf);
}

ATF_TC_WITHOUT_HEAD(writecount_seal);
ATF_TC_BODY(writecount_seal, tc)
{
	char *p, *q;
	size_t pg;
	int fd;

	pg = pagesize();
	fd = memfd_create("mremap", MFD_ALLOW_SEALING);
	ATF_REQUIRE(fd != -1);
	ATF_REQUIRE(ftruncate(fd, 2 * pg) == 0);

	/* A second mapping and a grown tail count as writable mappings. */
	p = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	q = mremap(p, 0, 2 * pg, MAYMOVE);
	ATF_REQUIRE(q != MAP_FAILED);
	ATF_REQUIRE(munmap(p, 2 * pg) == 0);
	ATF_CHECK_ERRNO(EBUSY, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1);
	p = mremap(q, 2 * pg, 3 * pg, MAYMOVE);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_CHECK_ERRNO(EBUSY, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1);
	ATF_REQUIRE(munmap(p, 3 * pg) == 0);
	ATF_CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0);
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC(wired);
ATF_TC_HEAD(wired, tc)
{
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(wired, tc)
{
	struct kinfo_vmentry kve;
	struct rlimit rl;
	char *extra, *p, *q, *r;
	size_t pg;

	pg = pagesize();
	rl.rlim_cur = rl.rlim_max = 4 * pg;
	ATF_REQUIRE(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);
	extra = map_anon(3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	p = map_anon(4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, 4 * pg, 130);
	ATF_REQUIRE_MSG(mlock(p, 4 * pg) == 0, "mlock: %s", strerror(errno));
	ATF_CHECK_ERRNO(ENOMEM, mlock(extra, pg) == -1);

	/*
	 * The moved pages stay wired, and they count against
	 * RLIMIT_MEMLOCK as before the move.
	 */
	q = map_anon(4 * pg, PROT_NONE, MAP_PRIVATE);
	ATF_REQUIRE(mremap(p, 4 * pg, 4 * pg, FIXED, q) == q);
	ATF_CHECK(check(q, 4 * pg, 130));
	ATF_REQUIRE_EQ(1, entries(q, 4 * pg, &kve));
	ATF_CHECK((kve.kve_flags & KVME_FLAG_USER_WIRED) != 0);
	ATF_CHECK_ERRNO(ENOMEM, mlock(extra, pg) == -1);

	/* Growth past RLIMIT_MEMLOCK, then within it. */
	ATF_CHECK_ERRNO(EAGAIN,
	    mremap(q, 4 * pg, 5 * pg, MAYMOVE) == MAP_FAILED);
	rl.rlim_cur = rl.rlim_max = 8 * pg;
	ATF_REQUIRE(setrlimit(RLIMIT_MEMLOCK, &rl) == 0);
	r = mremap(q, 4 * pg, 6 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(r != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(check(r, 4 * pg, 130));
	ATF_CHECK(entries(r + 4 * pg, 2 * pg, &kve) >= 1 &&
	    (kve.kve_flags & KVME_FLAG_USER_WIRED) != 0);
	ATF_CHECK(mlock(extra, 2 * pg) == 0);
	ATF_CHECK_ERRNO(ENOMEM, mlock(extra + 2 * pg, pg) == -1);

	/* A shrink unwires the tail. */
	ATF_REQUIRE(mremap(r, 6 * pg, 3 * pg, 0) == r);
	ATF_CHECK(mlock(extra + 2 * pg, pg) == 0);
	ATF_CHECK(munlock(r, 3 * pg) == 0);
	ATF_CHECK(munlock(extra, 3 * pg) == 0);
}

ATF_TC_WITHOUT_HEAD(rlimit_vmem);
ATF_TC_BODY(rlimit_vmem, tc)
{
	struct kinfo_proc *kp;
	struct rlimit rl;
	char *p, *q, *s;
	size_t pg;

	pg = pagesize();
	p = map_anon(pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	s = map_anon(pg, PROT_READ | PROT_WRITE, MAP_SHARED);
	fill(p, pg, 140);
	kp = kinfo_getproc(getpid());
	ATF_REQUIRE(kp != NULL);
	ATF_REQUIRE(getrlimit(RLIMIT_VMEM, &rl) == 0);
	rl.rlim_cur = kp->ki_size + 8 * pg;
	free(kp);
	ATF_REQUIRE(setrlimit(RLIMIT_VMEM, &rl) == 0);
	ATF_CHECK_ERRNO(ENOMEM,
	    mremap(p, pg, 64 * pg, MAYMOVE) == MAP_FAILED);
	ATF_CHECK_ERRNO(ENOMEM, mremap(s, 0, 64 * pg, MAYMOVE) == MAP_FAILED);
	q = mremap(p, pg, 2 * pg, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(check(q, pg, 140));
}

ATF_TC_WITHOUT_HEAD(fork_grow);
ATF_TC_BODY(fork_grow, tc)
{
	char *p;
	size_t pg;
	pid_t pid;
	int status;

	pg = pagesize();
	p = map_with_room(pg, 3 * pg);
	fill(p, pg, 150);
	pid = fork();
	ATF_REQUIRE(pid != -1);
	if (pid == 0) {
		/*
		 * The inherited entry is copy-on-write; the first growth
		 * adds a second entry, and the second growth spans both.
		 */
		if (mremap(p, pg, 2 * pg, 0) != p)
			_exit(1);
		fill(p + pg, pg, 151);
		if (mremap(p, 2 * pg, 4 * pg, 0) != p)
			_exit(2);
		if (!check(p, 2 * pg, 150) || !zero(p + 2 * pg, 2 * pg))
			_exit(3);
		fill(p, 4 * pg, 160);
		if (mremap(p, 4 * pg, pg, 0) != p || !check(p, pg, 160))
			_exit(4);
		_exit(0);
	}
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	ATF_CHECK(WIFEXITED(status));
	ATF_CHECK_EQ(0, WEXITSTATUS(status));
	ATF_CHECK(check(p, pg, 150));
	ATF_CHECK(unmapped(p + pg, 3 * pg));
}

static size_t
superpage_size(void)
{
	size_t ps[MAXPAGESIZES];
	int n;

	n = getpagesizes(ps, MAXPAGESIZES);
	return (n > 1 ? ps[1] : 0);
}

ATF_TC_WITHOUT_HEAD(largepage);
ATF_TC_BODY(largepage, tc)
{
	char *p, *q, *r;
	size_t lp;
	int fd;

	lp = superpage_size();
	if (lp == 0)
		atf_tc_skip("no large pages");
	fd = shm_create_largepage(SHM_ANON, O_RDWR, 1,
	    SHM_LARGEPAGE_ALLOC_DEFAULT, 0600);
	ATF_REQUIRE_MSG(fd != -1, "shm_create_largepage: %s",
	    strerror(errno));
	if (ftruncate(fd, 2 * lp) != 0)
		atf_tc_skip("ftruncate: %s", strerror(errno));
	p = mmap(NULL, lp, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	p[0] = 1;
	p[lp - 1] = 2;

	/* Sizes and addresses are multiples of the large page. */
	ATF_CHECK_ERRNO(EINVAL,
	    mremap(p, lp, lp + pagesize(), MAYMOVE) == MAP_FAILED);
	ATF_CHECK_ERRNO(EINVAL, mremap(p, lp / 2, lp, MAYMOVE) == MAP_FAILED);
	q = mremap(p, lp, 2 * lp, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(((uintptr_t)q & (lp - 1)) == 0);
	ATF_CHECK(q[0] == 1 && q[lp - 1] == 2);
	q[lp] = 3;
	r = mremap(q, 0, 2 * lp, MAYMOVE);
	ATF_REQUIRE_MSG(r != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(((uintptr_t)r & (lp - 1)) == 0);
	ATF_CHECK(r[0] == 1 && r[lp] == 3);
	ATF_CHECK(mremap(q, 2 * lp, lp, 0) == q);
	ATF_REQUIRE(close(fd) == 0);
}

ATF_TC_WITHOUT_HEAD(superpage);
ATF_TC_BODY(superpage, tc)
{
	char *p, *q;
	size_t sp;

	sp = superpage_size();
	if (sp == 0)
		atf_tc_skip("no superpages");
	p = mmap(NULL, 2 * sp, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE | MAP_ALIGNED_SUPER, -1, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	fill(p, 2 * sp, 170);

	/* Block growth in place, so that the mapping moves. */
	ATF_REQUIRE(mmap(p + 2 * sp, pagesize(), PROT_NONE,
	    MAP_GUARD | MAP_FIXED | MAP_EXCL, -1, 0) != MAP_FAILED);
	q = mremap(p, 2 * sp, 4 * sp, MAYMOVE);
	ATF_REQUIRE_MSG(q != MAP_FAILED, "mremap: %s", strerror(errno));
	ATF_CHECK(q != p);

	/* The pages keep their superpage alignment. */
	ATF_CHECK(((uintptr_t)q & (sp - 1)) == 0);
	ATF_CHECK(check(q, 2 * sp, 170));
	fill(q, 4 * sp, 180);
	ATF_CHECK(check(q, 4 * sp, 180));
}

ATF_TC_WITHOUT_HEAD(capability_mode);
ATF_TC_BODY(capability_mode, tc)
{
	char *p, *q;
	size_t pg;
	pid_t pid;
	int status;

	pg = pagesize();
	p = map_anon(pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, pg, 190);
	pid = fork();
	ATF_REQUIRE(pid != -1);
	if (pid == 0) {
		if (cap_enter() != 0)
			_exit(1);
		q = mremap(p, pg, 4 * pg, MAYMOVE);
		_exit(q != MAP_FAILED && check(q, pg, 190) ? 0 : 2);
	}
	ATF_REQUIRE(waitpid(pid, &status, 0) == pid);
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

ATF_TC_WITHOUT_HEAD(fifth_argument);
ATF_TC_BODY(fifth_argument, tc)
{
	char *p, *q;
	size_t pg;

	/* Without MREMAP_FIXED, the kernel ignores the fifth argument. */
	pg = pagesize();
	p = map_anon(pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, pg, 200);
	q = (char *)(uintptr_t)__syscall(SYS_mremap, p, pg, pg, MAYMOVE,
	    (uintptr_t)0xdeadbeef1);
	ATF_CHECK(q == p);
	q = (char *)(uintptr_t)__syscall(SYS_mremap, p, pg, 64 * pg, MAYMOVE,
	    (uintptr_t)0xdeadbeef1);
	ATF_REQUIRE(q != MAP_FAILED);
	ATF_CHECK(check(q, pg, 200));
}

#define	STRESS_THREADS	4
#define	STRESS_SECONDS	5

static volatile bool stress_done;

static void *
stress_worker(void *arg)
{
	char *p, *q;
	size_t len, newlen, pg;
	unsigned int seed;
	int id, n;

	id = (int)(uintptr_t)arg;
	seed = id;
	pg = pagesize();
	len = pg;
	p = map_anon(len, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	fill(p, len, id);
	for (n = 0; !stress_done; n++) {
		newlen = (1 + rand_r(&seed) % 16) * pg;
		switch (rand_r(&seed) % 4) {
		case 0:
			q = mremap(p, len, newlen, MAYMOVE);
			break;
		case 1:
			q = mremap(p, len, len, DONTUNMAP);
			if (q != MAP_FAILED && munmap(p, len) != 0)
				return ((void *)1);
			newlen = len;
			break;
		case 2:
			q = mremap(p, len, newlen, 0);
			if (q == MAP_FAILED && errno == ENOMEM) {
				q = p;
				newlen = len;
			}
			break;
		default:
			q = mremap(p, len, MIN(len, newlen), 0);
			newlen = MIN(len, newlen);
			break;
		}
		if (q == MAP_FAILED)
			return ((void *)2);
		if (!check(q, MIN(len, newlen), id + n))
			return ((void *)3);
		p = q;
		len = newlen;
		fill(p, len, id + n + 1);
	}
	return (NULL);
}

/* Change other mappings of the process while the workers run. */
static void *
stress_churn(void *arg __unused)
{
	char *p;
	size_t pg;

	pg = pagesize();
	while (!stress_done) {
		p = map_anon(8 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE);
		fill(p, 8 * pg, 1);
		(void)mprotect(p + pg, pg, PROT_READ);
		(void)madvise(p, 8 * pg, MADV_FREE);
		(void)munmap(p, 8 * pg);
	}
	return (NULL);
}

ATF_TC_WITHOUT_HEAD(threads);
ATF_TC_BODY(threads, tc)
{
	pthread_t churn, workers[STRESS_THREADS];
	void *res;
	size_t len;
	int check_all, check_old, i;
	bool restore;

	/* With root, check the whole map after each change. */
	check_all = 2;
	len = sizeof(check_old);
	restore = geteuid() == 0 && sysctlbyname("debug.vmmap_check",
	    &check_old, &len, &check_all, sizeof(check_all)) == 0;
	stress_done = false;
	for (i = 0; i < STRESS_THREADS; i++)
		ATF_REQUIRE(pthread_create(&workers[i], NULL, stress_worker,
		    (void *)(uintptr_t)(i * 37)) == 0);
	ATF_REQUIRE(pthread_create(&churn, NULL, stress_churn, NULL) == 0);
	sleep(STRESS_SECONDS);
	stress_done = true;
	ATF_REQUIRE(pthread_join(churn, NULL) == 0);
	for (i = 0; i < STRESS_THREADS; i++) {
		ATF_REQUIRE(pthread_join(workers[i], &res) == 0);
		ATF_CHECK_MSG(res == NULL, "worker %d failed: %p", i, res);
	}
	if (restore)
		(void)sysctlbyname("debug.vmmap_check", NULL, NULL, &check_old,
		    sizeof(check_old));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, einval);
	ATF_TP_ADD_TC(tp, einval_entries);
	ATF_TP_ADD_TC(tp, efault);
	ATF_TP_ADD_TC(tp, enomem);
	ATF_TP_ADD_TC(tp, same_size);
	ATF_TP_ADD_TC(tp, shrink);
	ATF_TP_ADD_TC(tp, grow_in_place);
	ATF_TP_ADD_TC(tp, grow_move);
	ATF_TP_ADD_TC(tp, move_protection);
	ATF_TP_ADD_TC(tp, fixed);
	ATF_TP_ADD_TC(tp, dontunmap_private);
	ATF_TP_ADD_TC(tp, dontunmap_shared);
	ATF_TP_ADD_TC(tp, copy_shared_anon);
	ATF_TP_ADD_TC(tp, copy_shm);
	ATF_TP_ADD_TC(tp, file_shared);
	ATF_TP_ADD_TC(tp, file_private);
	ATF_TP_ADD_TC(tp, writecount_text);
	ATF_TP_ADD_TC(tp, writecount_seal);
	ATF_TP_ADD_TC(tp, wired);
	ATF_TP_ADD_TC(tp, rlimit_vmem);
	ATF_TP_ADD_TC(tp, fork_grow);
	ATF_TP_ADD_TC(tp, largepage);
	ATF_TP_ADD_TC(tp, superpage);
	ATF_TP_ADD_TC(tp, capability_mode);
	ATF_TP_ADD_TC(tp, fifth_argument);
	ATF_TP_ADD_TC(tp, threads);
	return (atf_no_error());
}
