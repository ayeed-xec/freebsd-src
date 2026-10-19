#!/bin/sh
#
# Copyright (c) 2026 Ayeed Shaikh <Ayeed.AI10@race.reva.edu.in>
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#

fs_head()
{
	atf_set "require.user" "root"
	atf_set "require.progs" "mdconfig newfs mount umount"
	atf_set "timeout" "180"
}

disk()
{
	atf_check truncate -s 64m image
	atf_check -o save:md.unit mdconfig -a -t vnode -f "$(pwd)/image"
	device="/dev/$(cat md.unit)"
	atf_check mkdir mnt
}

ufs_mount()
{
	disk
	case " $* " in
	*" -j "*)
		atf_check -o ignore -e match:'soft updates journaling set' \
		    newfs "$@" "$device"
		;;
	*)
		atf_check -o ignore newfs "$@" "$device"
		;;
	esac
	atf_check mount "$device" "$(pwd)/mnt"
	atf_check chmod 0777 mnt
}

cases()
{
	test="$(atf_get_srcdir)/o_tmpfile_test"
	for tc in basic mode writeonly invalid paths empty_path \
	    removed_directory immutable fhopen_reject link_name exclusive \
	    capsicum ordinary ordinary_removed threads; do
		atf_check -o ignore -e ignore "$test" -r "$(pwd)/result" \
		    -v directory="$(pwd)/mnt" "$tc"
		atf_check -o match:'^passed$' cat result
	done
}

fs_cleanup()
{
	for name in mnt lower; do
		if mount -p | awk -v p="$(pwd)/$name" \
		    '$2 == p { found = 1 } END { exit !found }'; then
			umount -f "$(pwd)/$name"
		fi
	done
	if [ -f md.unit ]; then
		mdconfig -d -u "$(cat md.unit)"
	fi
}

atf_test_case tmpfs cleanup
tmpfs_head() { fs_head; }
tmpfs_body()
{
	atf_check mkdir mnt
	atf_check mount -t tmpfs tmpfs "$(pwd)/mnt"
	cases
}
tmpfs_cleanup() { fs_cleanup; }

atf_test_case ufs cleanup
ufs_head() { fs_head; }
ufs_body()
{
	ufs_mount -O 2
	cases
}
ufs_cleanup() { fs_cleanup; }

atf_test_case softupdates cleanup
softupdates_head() { fs_head; }
softupdates_body()
{
	ufs_mount -O 2 -U
	cases
}
softupdates_cleanup() { fs_cleanup; }

atf_test_case nullfs cleanup
nullfs_head() { fs_head; }
nullfs_body()
{
	atf_check mkdir lower mnt
	atf_check mount -t tmpfs tmpfs "$(pwd)/lower"
	atf_check mount -t nullfs "$(pwd)/lower" "$(pwd)/mnt"
	cases
}
nullfs_cleanup() { fs_cleanup; }

atf_test_case suj cleanup
suj_head() { fs_head; }
suj_body()
{
	ufs_mount -O 2 -j
	atf_check "$(atf_get_srcdir)/o_tmpfile_helper" error "$(pwd)/mnt" 45
}
suj_cleanup() { fs_cleanup; }

atf_test_case msdosfs cleanup
msdosfs_head() { fs_head; }
msdosfs_body()
{
	disk
	atf_check -o ignore newfs_msdos -h 1 -u 32 "$device"
	atf_check mount -t msdosfs "$device" "$(pwd)/mnt"
	atf_check "$(atf_get_srcdir)/o_tmpfile_helper" error "$(pwd)/mnt" 45
}
msdosfs_cleanup() { fs_cleanup; }

atf_test_case readonly cleanup
readonly_head() { fs_head; }
readonly_body()
{
	ufs_mount -O 2
	atf_check mount -u -o ro "$(pwd)/mnt"
	atf_check "$(atf_get_srcdir)/o_tmpfile_helper" error "$(pwd)/mnt" 30
}
readonly_cleanup() { fs_cleanup; }

atf_test_case no_inodes cleanup
no_inodes_head() { fs_head; }
no_inodes_body()
{
	atf_check mkdir mnt
	atf_check mount -t tmpfs -o inodes=16 tmpfs "$(pwd)/mnt"
	atf_check "$(atf_get_srcdir)/o_tmpfile_helper" exhaust "$(pwd)/mnt" 28
}
no_inodes_cleanup() { fs_cleanup; }

atf_test_case inode_quota cleanup
inode_quota_head() { fs_head; }
inode_quota_body()
{
	ufs_mount -O 2
	atf_check -e ignore "$(atf_get_srcdir)/o_tmpfile_helper" quota \
	    "$(pwd)/mnt"
}
inode_quota_cleanup() { fs_cleanup; }

atf_test_case crash cleanup
crash_head() { fs_head; }
crash_body()
{
	ufs_mount -O 2 -U
	atf_check "$(atf_get_srcdir)/o_tmpfile_helper" crash "$(pwd)/mnt" \
	    "$device" "$(pwd)/crash.img"
	atf_check -o save:fsck.log fsck_ffs -fy crash.img
	atf_check -o match:'UNREF FILE' cat fsck.log
	atf_check -o match:'CLEAR\? yes' cat fsck.log
	atf_check -o save:check.log fsck_ffs -fn crash.img
	atf_check -o match:'^2 files, 2 used,' cat check.log
	atf_check -s exit:1 grep -E 'UNREF FILE|CLEAR\?|SALVAGE\?|MODIFIED' \
	    check.log
}
crash_cleanup() { fs_cleanup; }

atf_init_test_cases()
{
	atf_add_test_case tmpfs
	atf_add_test_case ufs
	atf_add_test_case softupdates
	atf_add_test_case nullfs
	atf_add_test_case suj
	atf_add_test_case msdosfs
	atf_add_test_case readonly
	atf_add_test_case no_inodes
	atf_add_test_case inode_quota
	atf_add_test_case crash
}
