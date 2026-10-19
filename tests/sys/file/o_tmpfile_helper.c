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

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ufs/ufs/quota.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void exhaust(const char *, int);
static void quota(const char *);
static void crash_image(const char *, const char *, const char *);

static void
exhaust(const char *path, int expected)
{
	int fds[512];
	int fd, i, n;

	for (n = 0; n < (int)nitems(fds); n++) {
		fds[n] = open(path, O_TMPFILE | O_RDWR, 0600);
		if (fds[n] == -1)
			break;
	}
	if (n == 0 || n == (int)nitems(fds) || errno != expected)
		err(1, "exhaust: count %d, expected errno %d", n, expected);
	if (close(fds[--n]) == -1)
		err(1, "close");
	sync();
	fd = open(path, O_TMPFILE | O_RDWR, 0600);
	if (fd == -1)
		err(1, "inode was not released at close");
	if (close(fd) == -1)
		err(1, "close");
	for (i = 0; i < n; i++) {
		if (close(fds[i]) == -1)
			err(1, "close");
	}
}

static void
quota(const char *path)
{
	char qpath[PATH_MAX];
	struct dqblk dq;
	struct passwd *pw;
	pid_t pid;
	int fd, status;

	pw = getpwnam("nobody");
	if (pw == NULL)
		err(1, "getpwnam");
	if (snprintf(qpath, sizeof(qpath), "%s/quota.user", path) >=
	    (int)sizeof(qpath))
		errx(1, "quota path too long");
	fd = open(qpath, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd == -1 || close(fd) == -1)
		err(1, "create quota file");
	if (quotactl(path, QCMD(Q_QUOTAON, USRQUOTA), 0, qpath) == -1)
		err(1, "Q_QUOTAON");
	memset(&dq, 0, sizeof(dq));
	dq.dqb_ihardlimit = 4;
	if (quotactl(path, QCMD(Q_SETQUOTA, USRQUOTA), pw->pw_uid,
	    &dq) == -1)
		err(1, "Q_SETQUOTA");
	pid = fork();
	if (pid == -1)
		err(1, "fork");
	if (pid == 0) {
		if (chdir(path) == -1 || setgid(pw->pw_gid) == -1 ||
		    setuid(pw->pw_uid) == -1)
			err(1, "drop privileges");
		exhaust(".", EDQUOT);
		exit(0);
	}
	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid");
	if (quotactl(path, QCMD(Q_QUOTAOFF, USRQUOTA), 0, NULL) == -1)
		err(1, "Q_QUOTAOFF");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		errx(1, "quota child failed: %d", status);
}

static void
crash_image(const char *path, const char *device, const char *image)
{
	char buf[65536];
	ssize_t n, off, written;
	int fd, in, out;

	fd = open(path, O_TMPFILE | O_RDWR, 0600);
	if (fd == -1 || write(fd, "unnamed", 7) != 7 || fsync(fd) == -1)
		err(1, "persist unnamed file");
	sync();
	in = open(device, O_RDONLY);
	out = open(image, O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (in == -1 || out == -1)
		err(1, "open image");
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		for (off = 0; off < n; off += written) {
			written = write(out, buf + off, n - off);
			if (written <= 0)
				err(1, "write image");
		}
	}
	if (n == -1 || fsync(out) == -1)
		err(1, "copy image");
	if (close(out) == -1 || close(in) == -1 || close(fd) == -1)
		err(1, "close image");
}

int
main(int argc, char **argv)
{
	int expected, fd;

	if (argc == 4 && strcmp(argv[1], "error") == 0) {
		expected = atoi(argv[3]);
		fd = open(argv[2], O_TMPFILE | O_RDWR, 0600);
		if (fd != -1 || errno != expected)
			err(1, "open returned %d, expected errno %d",
			    fd, expected);
	} else if (argc == 4 && strcmp(argv[1], "exhaust") == 0) {
		exhaust(argv[2], atoi(argv[3]));
	} else if (argc == 3 && strcmp(argv[1], "quota") == 0) {
		quota(argv[2]);
	} else if (argc == 5 && strcmp(argv[1], "crash") == 0) {
		crash_image(argv[2], argv[3], argv[4]);
	} else {
		errx(1, "usage: %s error|exhaust path errno; quota path; "
		    "crash path device image", argv[0]);
	}
	return (0);
}
