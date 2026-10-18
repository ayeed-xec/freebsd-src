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
 * Tests of F_SEAL_FUTURE_WRITE, SEEK_DATA and SEEK_HOLE on shm objects, and
 * the name of an anonymous object in kinfo_file.
 */

#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/user.h>

#include <atf-c.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static int
sealable_memfd(const char *name, off_t size)
{
	int fd;

	fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
	ATF_REQUIRE_MSG(fd >= 0, "memfd_create: %s", strerror(errno));
	ATF_REQUIRE(ftruncate(fd, size) == 0);
	return (fd);
}

ATF_TC_WITHOUT_HEAD(future_write_blocks_new_writes);
ATF_TC_BODY(future_write_blocks_new_writes, tc)
{
	char *p;
	int fd, pgsz;

	pgsz = getpagesize();
	fd = sealable_memfd("fw", pgsz);
	ATF_REQUIRE(fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
	ATF_REQUIRE((fcntl(fd, F_GET_SEALS) & F_SEAL_FUTURE_WRITE) != 0);

	ATF_REQUIRE_ERRNO(EPERM, write(fd, "x", 1) == -1);
	ATF_REQUIRE_ERRNO(EPERM, pwrite(fd, "x", 1, 0) == -1);
	ATF_REQUIRE_ERRNO(EACCES, mmap(NULL, pgsz, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0) == MAP_FAILED);

	/* A read-only shared mapping works, and cannot become writable. */
	p = mmap(NULL, pgsz, PROT_READ, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_REQUIRE_ERRNO(EACCES, mprotect(p, pgsz, PROT_READ | PROT_WRITE) ==
	    -1);
	ATF_REQUIRE(munmap(p, pgsz) == 0);

	/* A private mapping is a copy, so it can be written. */
	p = mmap(NULL, pgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	p[0] = 'p';
	ATF_REQUIRE(munmap(p, pgsz) == 0);
}

ATF_TC_WITHOUT_HEAD(future_write_keeps_old_mapping);
ATF_TC_BODY(future_write_keeps_old_mapping, tc)
{
	char buf[4], *p;
	int fd, pgsz;

	pgsz = getpagesize();
	fd = sealable_memfd("old", pgsz);
	p = mmap(NULL, pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);

	/* Unlike F_SEAL_WRITE, the seal is allowed with a writable mapping. */
	ATF_REQUIRE(fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
	memcpy(p, "abc", 3);
	ATF_REQUIRE_EQ(3, pread(fd, buf, 3, 0));
	ATF_REQUIRE(memcmp(buf, "abc", 3) == 0);
	ATF_REQUIRE(munmap(p, pgsz) == 0);
}

ATF_TC_WITHOUT_HEAD(seal_write_unchanged);
ATF_TC_BODY(seal_write_unchanged, tc)
{
	char *p;
	int fd, pgsz;

	pgsz = getpagesize();
	fd = sealable_memfd("sw", pgsz);
	p = mmap(NULL, pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ATF_REQUIRE(p != MAP_FAILED);
	ATF_REQUIRE_ERRNO(EBUSY, fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1);
	ATF_REQUIRE(munmap(p, pgsz) == 0);
	ATF_REQUIRE(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0);
	ATF_REQUIRE_ERRNO(EPERM, write(fd, "x", 1) == -1);
}

ATF_TC_WITHOUT_HEAD(future_write_after_seal_seal);
ATF_TC_BODY(future_write_after_seal_seal, tc)
{
	int fd;

	fd = sealable_memfd("ss", getpagesize());
	ATF_REQUIRE(fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0);
	ATF_REQUIRE_ERRNO(EPERM, fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) ==
	    -1);
}

ATF_TC_WITHOUT_HEAD(future_write_blocks_dealloc);
ATF_TC_BODY(future_write_blocks_dealloc, tc)
{
	struct spacectl_range range;
	int fd;

	fd = sealable_memfd("dealloc", getpagesize());
	ATF_REQUIRE(fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
	range.r_offset = 0;
	range.r_len = getpagesize();
	ATF_REQUIRE_ERRNO(EPERM, fspacectl(fd, SPACECTL_DEALLOC, &range, 0,
	    &range) == -1);
}

ATF_TC_WITHOUT_HEAD(seek_data_hole);
ATF_TC_BODY(seek_data_hole, tc)
{
	off_t pgsz;
	int fd;

	pgsz = getpagesize();
	fd = sealable_memfd("seek", 4 * pgsz);

	/* An empty object is one hole. */
	ATF_REQUIRE_ERRNO(ENXIO, lseek(fd, 0, SEEK_DATA) == -1);
	ATF_REQUIRE_EQ(0, lseek(fd, 0, SEEK_HOLE));

	/* Data in pages 1 and 2, holes in pages 0 and 3. */
	ATF_REQUIRE_EQ(1, pwrite(fd, "a", 1, pgsz));
	ATF_REQUIRE_EQ(1, pwrite(fd, "b", 1, 2 * pgsz + 10));
	ATF_REQUIRE_EQ(pgsz, lseek(fd, 0, SEEK_DATA));
	ATF_REQUIRE_EQ(pgsz + 5, lseek(fd, pgsz + 5, SEEK_DATA));
	ATF_REQUIRE_EQ(3 * pgsz, lseek(fd, pgsz, SEEK_HOLE));
	ATF_REQUIRE_EQ(5, lseek(fd, 5, SEEK_HOLE));
	ATF_REQUIRE_ERRNO(ENXIO, lseek(fd, 3 * pgsz, SEEK_DATA) == -1);
	ATF_REQUIRE_EQ(3 * pgsz + 1, lseek(fd, 3 * pgsz + 1, SEEK_HOLE));

	/* At or past the end. */
	ATF_REQUIRE_ERRNO(ENXIO, lseek(fd, 4 * pgsz, SEEK_DATA) == -1);
	ATF_REQUIRE_ERRNO(ENXIO, lseek(fd, 4 * pgsz, SEEK_HOLE) == -1);
	ATF_REQUIRE_ERRNO(ENXIO, lseek(fd, -1, SEEK_DATA) == -1);

	/* All data: the hole is the end of the object. */
	ATF_REQUIRE_EQ(1, pwrite(fd, "c", 1, 0));
	ATF_REQUIRE_EQ(1, pwrite(fd, "d", 1, 3 * pgsz));
	ATF_REQUIRE_EQ(4 * pgsz, lseek(fd, 0, SEEK_HOLE));
}

static void
get_kinfo(int fd, struct kinfo_file *kif)
{
	memset(kif, 0, sizeof(*kif));
	kif->kf_structsize = sizeof(*kif);
	ATF_REQUIRE(fcntl(fd, F_KINFO, kif) == 0);
	ATF_REQUIRE_EQ(KF_TYPE_SHM, kif->kf_type);
}

ATF_TC_WITHOUT_HEAD(memfd_name);
ATF_TC_BODY(memfd_name, tc)
{
	struct kinfo_file kif;
	struct stat sb;
	int fd;

	fd = sealable_memfd("hello", getpagesize());
	get_kinfo(fd, &kif);
	ATF_REQUIRE_STREQ("memfd:hello", kif.kf_path);
	ATF_REQUIRE(fstat(fd, &sb) == 0);
	ATF_REQUIRE_EQ((uint64_t)sb.st_ino, kif.kf_un.kf_file.kf_file_fileid);
	ATF_REQUIRE(close(fd) == 0);

	/* An anonymous object without a name has no path. */
	fd = shm_open(SHM_ANON, O_RDWR, 0600);
	ATF_REQUIRE(fd >= 0);
	get_kinfo(fd, &kif);
	ATF_REQUIRE_STREQ("", kif.kf_path);
	ATF_REQUIRE(close(fd) == 0);

	/* A name that cannot be read is left out; the call still works. */
	fd = syscall(SYS_shm_open2, SHM_ANON, O_RDWR, 0600, 0,
	    (const char *)1);
	ATF_REQUIRE(fd >= 0);
	get_kinfo(fd, &kif);
	ATF_REQUIRE_STREQ("", kif.kf_path);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, future_write_blocks_new_writes);
	ATF_TP_ADD_TC(tp, future_write_keeps_old_mapping);
	ATF_TP_ADD_TC(tp, seal_write_unchanged);
	ATF_TP_ADD_TC(tp, future_write_after_seal_seal);
	ATF_TP_ADD_TC(tp, future_write_blocks_dealloc);
	ATF_TP_ADD_TC(tp, seek_data_hole);
	ATF_TP_ADD_TC(tp, memfd_name);
	return (atf_no_error());
}
