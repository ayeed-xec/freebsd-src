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
 * Tests of SOCK_ATOMIC: PF_LOCAL SOCK_SEQPACKET sockets that send one
 * record for each output operation and drop the end of a long record.
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	SOCK_PATH	"sock"

static void
atomic_pair(int *sv)
{
	ATF_REQUIRE_MSG(socketpair(PF_LOCAL, SOCK_SEQPACKET | SOCK_ATOMIC, 0,
	    sv) == 0, "socketpair: %s", strerror(errno));
}

static int
atomic_listener(int type)
{
	struct sockaddr_un sun;
	int s;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	sun.sun_len = sizeof(sun);
	strlcpy(sun.sun_path, SOCK_PATH, sizeof(sun.sun_path));
	s = socket(PF_LOCAL, type, 0);
	ATF_REQUIRE(s >= 0);
	ATF_REQUIRE(bind(s, (struct sockaddr *)&sun, sizeof(sun)) == 0);
	ATF_REQUIRE(listen(s, -1) == 0);
	return (s);
}

static int
connect_to(int type)
{
	struct sockaddr_un sun;
	int s;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	sun.sun_len = sizeof(sun);
	strlcpy(sun.sun_path, SOCK_PATH, sizeof(sun.sun_path));
	s = socket(PF_LOCAL, type, 0);
	ATF_REQUIRE(s >= 0);
	if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
		close(s);
		return (-1);
	}
	return (s);
}

static void
send_str(int s, const char *str)
{
	ssize_t n;

	n = send(s, str, strlen(str), 0);
	ATF_REQUIRE_MSG(n == (ssize_t)strlen(str), "send: %s",
	    strerror(errno));
}

static ssize_t
recv_flags(int s, char *buf, size_t len, int inflags, int *outflags)
{
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	iov.iov_base = buf;
	iov.iov_len = len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	n = recvmsg(s, &msg, inflags);
	*outflags = msg.msg_flags;
	return (n);
}

static void
check_records(int *sv)
{
	char buf[64];
	int flags;

	send_str(sv[0], "one");
	send_str(sv[0], "two!");
	send_str(sv[0], "three");
	ATF_REQUIRE_EQ(3, recv_flags(sv[1], buf, sizeof(buf), 0, &flags));
	ATF_REQUIRE(memcmp(buf, "one", 3) == 0);
	ATF_REQUIRE((flags & MSG_EOR) != 0);
	ATF_REQUIRE((flags & MSG_TRUNC) == 0);
	ATF_REQUIRE_EQ(4, recv(sv[1], buf, sizeof(buf), 0));
	ATF_REQUIRE(memcmp(buf, "two!", 4) == 0);
	ATF_REQUIRE_EQ(5, recv(sv[1], buf, sizeof(buf), 0));
	ATF_REQUIRE(memcmp(buf, "three", 5) == 0);
}

ATF_TC_WITHOUT_HEAD(records_socketpair);
ATF_TC_BODY(records_socketpair, tc)
{
	int sv[2];

	atomic_pair(sv);
	check_records(sv);
}

ATF_TC_WITHOUT_HEAD(records_accept);
ATF_TC_BODY(records_accept, tc)
{
	int l, sv[2];

	l = atomic_listener(SOCK_SEQPACKET | SOCK_ATOMIC);
	sv[0] = connect_to(SOCK_SEQPACKET | SOCK_ATOMIC);
	ATF_REQUIRE(sv[0] >= 0);
	sv[1] = accept(l, NULL, NULL);
	ATF_REQUIRE(sv[1] >= 0);
	check_records(sv);
	/* The accepted socket has the mode too. */
	check_records((int []){ sv[1], sv[0] });
}

ATF_TC_WITHOUT_HEAD(short_receive_truncates);
ATF_TC_BODY(short_receive_truncates, tc)
{
	char buf[64];
	int flags, sv[2];

	atomic_pair(sv);
	send_str(sv[0], "0123456789");
	send_str(sv[0], "xyz");
	ATF_REQUIRE_EQ(4, recv_flags(sv[1], buf, 4, 0, &flags));
	ATF_REQUIRE(memcmp(buf, "0123", 4) == 0);
	ATF_REQUIRE((flags & MSG_TRUNC) != 0);
	ATF_REQUIRE_EQ(3, recv_flags(sv[1], buf, sizeof(buf), 0, &flags));
	ATF_REQUIRE(memcmp(buf, "xyz", 3) == 0);
	ATF_REQUIRE((flags & MSG_TRUNC) == 0);
}

ATF_TC_WITHOUT_HEAD(trunc_full_length);
ATF_TC_BODY(trunc_full_length, tc)
{
	char buf[64];
	int sv[2];

	atomic_pair(sv);
	send_str(sv[0], "0123456789");
	ATF_REQUIRE_EQ(10, recv(sv[1], buf, 4, MSG_TRUNC));
	ATF_REQUIRE(memcmp(buf, "0123", 4) == 0);
	send_str(sv[0], "ab");
	ATF_REQUIRE_EQ(2, recv(sv[1], buf, sizeof(buf), MSG_TRUNC));
}

ATF_TC_WITHOUT_HEAD(peek_keeps_record);
ATF_TC_BODY(peek_keeps_record, tc)
{
	char buf[64];
	int flags, sv[2];

	atomic_pair(sv);
	send_str(sv[0], "0123456789");
	ATF_REQUIRE_EQ(4, recv_flags(sv[1], buf, 4, MSG_PEEK, &flags));
	ATF_REQUIRE((flags & MSG_TRUNC) != 0);
	ATF_REQUIRE_EQ(10, recv(sv[1], buf, sizeof(buf), MSG_PEEK | MSG_TRUNC));
	ATF_REQUIRE_EQ(10, recv(sv[1], buf, sizeof(buf), 0));
	ATF_REQUIRE(memcmp(buf, "0123456789", 10) == 0);
}

static void
send_fd(int s, int fd, const char *data, size_t len)
{
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;

	iov.iov_base = __DECONST(char *, data);
	iov.iov_len = len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	ATF_REQUIRE_EQ((ssize_t)len, sendmsg(s, &msg, 0));
}

static ssize_t
recv_fds(int s, char *buf, size_t len, int *nfds)
{
	char cbuf[CMSG_SPACE(2 * sizeof(int))];
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	iov.iov_base = buf;
	iov.iov_len = len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	n = recvmsg(s, &msg, 0);
	ATF_REQUIRE(n >= 0);
	*nfds = 0;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			*nfds += (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			close(*(int *)CMSG_DATA(cmsg));
		}
	}
	return (n);
}

ATF_TC_WITHOUT_HEAD(rights_stay_with_records);
ATF_TC_BODY(rights_stay_with_records, tc)
{
	char buf[2];
	int nfds, sv[2];

	atomic_pair(sv);
	send_fd(sv[0], STDIN_FILENO, "a", 1);
	send_fd(sv[0], STDIN_FILENO, "b", 1);
	ATF_REQUIRE_EQ(1, recv_fds(sv[1], buf, sizeof(buf), &nfds));
	ATF_REQUIRE_EQ('a', buf[0]);
	ATF_REQUIRE_EQ(1, nfds);
	ATF_REQUIRE_EQ(1, recv_fds(sv[1], buf, sizeof(buf), &nfds));
	ATF_REQUIRE_EQ('b', buf[0]);
	ATF_REQUIRE_EQ(1, nfds);
}

ATF_TC_WITHOUT_HEAD(control_only_record);
ATF_TC_BODY(control_only_record, tc)
{
	char buf[16];
	int nfds, sv[2];

	atomic_pair(sv);
	send_fd(sv[0], STDIN_FILENO, NULL, 0);
	send_str(sv[0], "data");
	ATF_REQUIRE_EQ(0, recv_fds(sv[1], buf, sizeof(buf), &nfds));
	ATF_REQUIRE_EQ(1, nfds);
	ATF_REQUIRE_EQ(4, recv_fds(sv[1], buf, sizeof(buf), &nfds));
	ATF_REQUIRE_EQ(0, nfds);
}

ATF_TC_WITHOUT_HEAD(empty_send);
ATF_TC_BODY(empty_send, tc)
{
	char buf[16];
	int sv[2];

	atomic_pair(sv);
	ATF_REQUIRE_EQ(0, send(sv[0], buf, 0, 0));
	ATF_REQUIRE_ERRNO(EAGAIN, recv(sv[1], buf, sizeof(buf),
	    MSG_DONTWAIT) == -1);
}

ATF_TC_WITHOUT_HEAD(emsgsize);
ATF_TC_BODY(emsgsize, tc)
{
	char *big;
	int size, sv[2];

	atomic_pair(sv);
	size = 4096;
	ATF_REQUIRE(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &size,
	    sizeof(size)) == 0);
	big = calloc(1, 8192);
	ATF_REQUIRE(big != NULL);
	ATF_REQUIRE_ERRNO(EMSGSIZE, send(sv[0], big, 8192, 0) == -1);

	/* Larger than the receive buffer of the peer. */
	size = 65536;
	ATF_REQUIRE(setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &size,
	    sizeof(size)) == 0);
	size = 2048;
	ATF_REQUIRE(setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &size,
	    sizeof(size)) == 0);
	ATF_REQUIRE_ERRNO(EMSGSIZE, send(sv[0], big, 4096, 0) == -1);
	ATF_REQUIRE_EQ(1024, send(sv[0], big, 1024, 0));
	free(big);
}

ATF_TC_WITHOUT_HEAD(eagain_whole_record);
ATF_TC_BODY(eagain_whole_record, tc)
{
	char buf[1000], rbuf[2000];
	int n, size, sv[2];

	atomic_pair(sv);
	size = 4096;
	ATF_REQUIRE(setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &size,
	    sizeof(size)) == 0);
	memset(buf, 'r', sizeof(buf));
	for (n = 0; send(sv[0], buf, sizeof(buf), MSG_DONTWAIT) ==
	    sizeof(buf); n++)
		;
	ATF_REQUIRE_EQ(EAGAIN, errno);
	ATF_REQUIRE(n > 0);
	/* Each queued record comes back whole: no send was split. */
	while (n-- > 0)
		ATF_REQUIRE_EQ((ssize_t)sizeof(buf), recv(sv[1], rbuf,
		    sizeof(rbuf), MSG_DONTWAIT));
	ATF_REQUIRE_ERRNO(EAGAIN, recv(sv[1], rbuf, sizeof(rbuf),
	    MSG_DONTWAIT) == -1);
}

struct sender {
	int	s;
	ssize_t	n;
};

static void *
sender_main(void *arg)
{
	struct sender *snd;
	char buf[3000];

	snd = arg;
	memset(buf, 'w', sizeof(buf));
	snd->n = send(snd->s, buf, sizeof(buf), 0);
	return (NULL);
}

ATF_TC_WITHOUT_HEAD(blocking_send_waits);
ATF_TC_BODY(blocking_send_waits, tc)
{
	struct sender snd;
	pthread_t thr;
	char buf[3000], rbuf[4000];
	int size, sv[2];

	atomic_pair(sv);
	size = 4096;
	ATF_REQUIRE(setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &size,
	    sizeof(size)) == 0);
	memset(buf, 'f', sizeof(buf));
	ATF_REQUIRE_EQ((ssize_t)sizeof(buf), send(sv[0], buf, sizeof(buf), 0));
	snd.s = sv[0];
	snd.n = 0;
	ATF_REQUIRE(pthread_create(&thr, NULL, sender_main, &snd) == 0);
	usleep(100000);
	ATF_REQUIRE_EQ(0, snd.n);
	ATF_REQUIRE_EQ(3000, recv(sv[1], rbuf, sizeof(rbuf), 0));
	ATF_REQUIRE(pthread_join(thr, NULL) == 0);
	ATF_REQUIRE_EQ(3000, snd.n);
	ATF_REQUIRE_EQ(3000, recv(sv[1], rbuf, sizeof(rbuf), 0));
	ATF_REQUIRE_EQ('w', rbuf[0]);
}

ATF_TC_WITHOUT_HEAD(creds_with_each_record);
ATF_TC_BODY(creds_with_each_record, tc)
{
	char buf[16], cbuf[1024];
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;
	int found, i, on, sv[2];

	atomic_pair(sv);
	on = 1;
	ATF_REQUIRE(setsockopt(sv[1], SOL_LOCAL, LOCAL_CREDS_PERSISTENT, &on,
	    sizeof(on)) == 0);
	send_str(sv[0], "a");
	send_str(sv[0], "b");
	for (i = 0; i < 2; i++) {
		iov.iov_base = buf;
		iov.iov_len = sizeof(buf);
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		ATF_REQUIRE_EQ(1, recvmsg(sv[1], &msg, 0));
		found = 0;
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
		    cmsg = CMSG_NXTHDR(&msg, cmsg))
			if (cmsg->cmsg_level == SOL_SOCKET &&
			    cmsg->cmsg_type == SCM_CREDS2)
				found++;
		ATF_REQUIRE_EQ(1, found);
	}
}

ATF_TC_WITHOUT_HEAD(eprototype);
ATF_TC_BODY(eprototype, tc)
{
	int l, s, sv[2];

	ATF_REQUIRE_ERRNO(EPROTOTYPE,
	    socket(PF_LOCAL, SOCK_STREAM | SOCK_ATOMIC, 0) == -1);
	ATF_REQUIRE_ERRNO(EPROTOTYPE,
	    socket(PF_LOCAL, SOCK_DGRAM | SOCK_ATOMIC, 0) == -1);
	ATF_REQUIRE_ERRNO(EPROTOTYPE,
	    socket(PF_INET, SOCK_SEQPACKET | SOCK_ATOMIC, 0) == -1);
	ATF_REQUIRE_ERRNO(EPROTOTYPE, socketpair(PF_LOCAL,
	    SOCK_STREAM | SOCK_ATOMIC, 0, sv) == -1);

	/* Both ends of a connection must have the same mode. */
	l = atomic_listener(SOCK_SEQPACKET | SOCK_ATOMIC);
	ATF_REQUIRE_EQ(-1, connect_to(SOCK_SEQPACKET));
	ATF_REQUIRE_EQ(EPROTOTYPE, errno);
	close(l);
	ATF_REQUIRE(unlink(SOCK_PATH) == 0);
	l = atomic_listener(SOCK_SEQPACKET);
	ATF_REQUIRE_EQ(-1, connect_to(SOCK_SEQPACKET | SOCK_ATOMIC));
	ATF_REQUIRE_EQ(EPROTOTYPE, errno);
	s = connect_to(SOCK_SEQPACKET);
	ATF_REQUIRE(s >= 0);
}

/*
 * Without SOCK_ATOMIC a record ends only at MSG_EOR, and a short receive
 * keeps the rest of the record (no change for native programs).
 */
ATF_TC_WITHOUT_HEAD(no_change_without_flag);
ATF_TC_BODY(no_change_without_flag, tc)
{
	char buf[64];
	int flags, sv[2];

	ATF_REQUIRE(socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sv) == 0);
	send_str(sv[0], "one");
	send_str(sv[0], "two!");
	send_str(sv[0], "three");
	ATF_REQUIRE_EQ(12, recv(sv[1], buf, sizeof(buf), 0));

	ATF_REQUIRE_EQ(10, send(sv[0], "0123456789", 10, MSG_EOR));
	ATF_REQUIRE_EQ(4, recv_flags(sv[1], buf, 4, 0, &flags));
	ATF_REQUIRE((flags & MSG_TRUNC) == 0);
	ATF_REQUIRE_EQ(6, recv_flags(sv[1], buf, sizeof(buf), 0, &flags));
	ATF_REQUIRE(memcmp(buf, "456789", 6) == 0);
	ATF_REQUIRE((flags & MSG_EOR) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, records_socketpair);
	ATF_TP_ADD_TC(tp, records_accept);
	ATF_TP_ADD_TC(tp, short_receive_truncates);
	ATF_TP_ADD_TC(tp, trunc_full_length);
	ATF_TP_ADD_TC(tp, peek_keeps_record);
	ATF_TP_ADD_TC(tp, rights_stay_with_records);
	ATF_TP_ADD_TC(tp, control_only_record);
	ATF_TP_ADD_TC(tp, empty_send);
	ATF_TP_ADD_TC(tp, emsgsize);
	ATF_TP_ADD_TC(tp, eagain_whole_record);
	ATF_TP_ADD_TC(tp, blocking_send_waits);
	ATF_TP_ADD_TC(tp, creds_with_each_record);
	ATF_TP_ADD_TC(tp, eprototype);
	ATF_TP_ADD_TC(tp, no_change_without_flag);
	return (atf_no_error());
}
