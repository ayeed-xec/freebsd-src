TESTSDIR=	${TESTSBASE}/sys/file

BINDIR=		${TESTSDIR}

ATF_TESTS_C+=	path_test
ATF_TESTS_C+=	closefrom_test
TAP_TESTS_C+=	dup_test
ATF_TESTS_C+=	fcntlflags_test
TAP_TESTS_SH+=	flock_test
PLAIN_TESTS_C+=	ftruncate_test
PLAIN_TESTS_C+=	newfileops_on_fork_test
ATF_TESTS_C+=	fspacectl_test
ATF_TESTS_C+=	o_tmpfile_test
ATF_TESTS_SH+=	o_tmpfile_fs_test
TEST_METADATA.o_tmpfile_fs_test+= is_exclusive="true"

PROGS+=		flock_helper
PROGS+=		o_tmpfile_helper

LIBADD.o_tmpfile_test=	pthread

LIBADD.closefrom_test=	util

LIBADD.flock_helper=	pthread

LIBADD.newfileops_on_fork_test=	pthread

.include <bsd.test.mk>
