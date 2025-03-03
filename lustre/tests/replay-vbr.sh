#!/bin/bash

set -e

MOUNT_2=${MOUNT_2:-"yes"}

LUSTRE=${LUSTRE:-$(dirname $0)/..}
. $LUSTRE/tests/test-framework.sh
init_test_env "$@"
init_logging

remote_mds_nodsh && log "SKIP: remote MDS with nodsh" && exit 0

ALWAYS_EXCEPT=" $REPLAY_VBR_EXCEPT"

#                                  ~6  (min)"
[ "$SLOW" = "no" ] && EXCEPT_SLOW="7 "

build_test_filter

check_and_setup_lustre

assert_DIR
rm -rf $DIR/[df][0-9]*

[ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE

# if there is no CLIENT1 defined, some tests can be ran on localhost
CLIENT1=${CLIENT1:-$HOSTNAME}
# if CLIENT2 doesn't exist then use CLIENT1 instead
# All tests should use CLIENT2 with MOUNT2 only therefore it will work if
# $CLIENT2 == CLIENT1
# Exception is the test which need two separate nodes
CLIENT2=${CLIENT2:-$CLIENT1}

is_mounted $MOUNT2 || error "MOUNT2 is not mounted"

#
# get_version(): Gets the version of an object on servers
# Parameter1: Client/Machine Name
# Parameter2: File Path
# Returns: Objectversion Or -1 if getobjversion fails.
#
get_version() {
	local var=${SINGLEMDS}_svc
	local client=$1
	local file=$2
	local fid=$(do_node $client $LFS path2fid $file)
	local objver=$(do_facet $SINGLEMDS $LCTL --device ${!var} \
		getobjversion \\\"$fid\\\")

	[[ -z $objver ]] && objver=-1
	echo $objver
}

#
# chk_get_version(): Wrapper to get_version().
# Parameter1: Client/Machine Name
# Parameter2: File Path
# Returns: Objectversion Or Exit with error in case objver is -1.
#
chk_get_version() {
	local objver=$(get_version $1 $2)

	[[ "$objver" == "-1" ]] && error "object version is empty."
	echo $objver
}


#save COS setting
cos_param_file=$TMP/rvbr-cos-params
save_lustre_params $(get_facets MDS) "mdt.*.commit_on_sharing" > $cos_param_file

# new sequence needed for MDS < v2_15_61-226-gf00d2467fc
if (( $MDS1_VERSION < $(version_code 2.15.61.226) )); then
	force_new_seq_all
fi

test_0a() {
	local ver=$(get_version $CLIENT1 $DIR/$tdir/1a)

	[[ "$ver" == "-1" ]] && return 0
	return 1
}
run_test 0a "getversion for non existent file shouldn't cause kernel panic"

test_0b() {
	local var=${SINGLEMDS}_svc
	local fid
	local file=$DIR/$tdir/f

	mkdir_on_mdt0 $DIR/$tdir
	touch $file
	fid=$($LFS path2fid $file)
	rm $file
	do_facet $SINGLEMDS $LCTL --device ${!var} getobjversion \\\"$fid\\\" || true
}
run_test 0b "getversion for non existent fid shouldn't cause kernel panic"

# test set #1: OPEN
test_1a() { # former test_0a
	local file=$DIR/$tfile
	local pre
	local post

	do_node $CLIENT1 mcreate $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 openfile -f O_RDWR $file
	post=$(chk_get_version $CLIENT1 $file)
	if (($pre != $post)); then
		error "version changed unexpectedly: pre $pre, post $post"
	fi
}
run_test 1a "open and close do not change versions"

test_1b() { # former test_0b
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"
	mkdir_on_mdt0 $MOUNT/$tdir

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 777 $MOUNT2/$tdir
	openfile -f O_RDWR:O_CREAT $MOUNT/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if ! $CHECKSTAT -a $DIR/$tdir/$tfile; then
		error_and_remount "open succeeded unexpectedly"
	fi
}
run_test 1b "open (O_CREAT) checks version of parent"

test_1c() { # former test_0c
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mkdir_on_mdt0 $DIR/$tdir
	chmod 755 $DIR/$tdir
	openfile -f O_RDWR:O_CREAT -m 0644 $DIR/$tdir/$tfile

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 0777 $MOUNT2/$tdir
	do_node $CLIENT2 chmod 0666 $MOUNT2/$tdir/$tfile
	rmultiop_start $CLIENT1 $DIR/$tdir/$tfile o_c
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_up $CLIENT1 || error "$CLIENT1 evicted"
	rmultiop_stop $CLIENT1 || error "close failed"
}
run_test 1c "open (non O_CREAT) does not checks versions"

# test set #2: CREAT (not open)
# - version of parent is not changed but checked
# - pre-version should be -1
# - post-version should be valid
test_2a() {  # extended former test_0d
	local pre
	local post

	# fifo
	pre=$(chk_get_version $CLIENT1 $DIR)
	do_node $CLIENT1 mkfifo $DIR/$tfile-fifo
	post=$(chk_get_version $CLIENT1 $DIR)
	if (($pre != $post)); then
		error "version was changed: pre $pre, post $post"
	fi
	# mkdir
	pre=$(chk_get_version $CLIENT1 $DIR)
	mkdir_on_mdt0 $DIR/$tfile-dir
	post=$(chk_get_version $CLIENT1 $DIR)
	if (($pre != $post)); then
		error "version was changed: pre $pre, post $post"
	fi
	rmdir $DIR/$tfile-dir

	# mknod
	pre=$(chk_get_version $CLIENT1 $DIR)
	mkfifo $DIR/$tfile-nod
	post=$(chk_get_version $CLIENT1 $DIR)
	if (($pre != $post)); then
		error "version was changed: pre $pre, post $post"
	fi
	# symlink
	pre=$(chk_get_version $CLIENT1 $DIR)
	mkfifo $DIR/$tfile-symlink
	post=$(chk_get_version $CLIENT1 $DIR)
	if (($pre != $post)); then
		error "version was changed: pre $pre, post $post"
	fi
	# remote directory
	if [ $MDSCOUNT -ge 2 ]; then
		#create remote dir
		local MDT_IDX=1
		pre=$(chk_get_version $CLIENT1 $DIR)
		$LFS mkdir -i $MDT_IDX $DIR/$tfile-remote_dir
		post=$(chk_get_version $CLIENT1 $DIR)
		if (($pre != $post)); then
			error "version was changed: pre $pre, post $post"
		fi
	fi
	rm -rf $DIR/$tfile-*
}
run_test 2a "create operations doesn't change version of parent"

test_2b() { # former test_0e
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mkdir_on_mdt0 $MOUNT/$tdir

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 777 $MOUNT2/$tdir
	mkfifo $DIR/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if ! $CHECKSTAT -a $DIR/$tdir/$tfile; then
		error_and_remount "create succeeded unexpectedly"
	fi
}
run_test 2b "create checks version of parent"

test_3a() { # former test_0f
	local pre
	local post

	do_node $CLIENT1 mcreate $DIR/$tfile
	pre=$(chk_get_version $CLIENT1 $DIR)
	do_node $CLIENT1 rm $DIR/$tfile
	post=$(chk_get_version $CLIENT1 $DIR)
	if (($pre != $post)); then
		error "version was changed: pre $pre, post $post"
	fi

	if [ $MDSCOUNT -ge 2 ]; then
		#create remote dir
		local MDT_IDX=1
		do_node $CLIENT1 $LFS mkdir -i $MDT_IDX $DIR/$tfile-remote_dir
		pre=$(chk_get_version $CLIENT1 $DIR)
		do_node $CLIENT1 rmdir $DIR/$tfile-remote_dir
		post=$(chk_get_version $CLIENT1 $DIR)
		if (($pre != $post)); then
			error "version was changed: pre $pre, post $post"
		fi
	fi
}
run_test 3a "unlink doesn't change version of parent"

test_3b() { # former test_0g
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mkdir_on_mdt0 $MOUNT/$tdir
	mcreate $DIR/$tdir/$tfile

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 777 $MOUNT2/$tdir
	rm $DIR/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if $CHECKSTAT -a $DIR/$tdir/$tfile; then
		error_and_remount "unlink succeeded unexpectedly"
	fi
}
run_test 3b "unlink checks version of parent"

test_4a() { # former test_0h
	local file=$DIR/$tfile
	local pre
	local post

	do_node $CLIENT1 mcreate $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 chown $RUNAS_ID:$RUNAS_GID $file
	post=$(chk_get_version $CLIENT1 $file)
	if (($pre == $post)); then
		error "version not changed: pre $pre, post $post"
	fi
}
run_test 4a "setattr of UID changes versions"

test_4b() { # former test_0i
	local file=$DIR/$tfile
	local pre
	local post

	do_node $CLIENT1 mcreate $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 chgrp $RUNAS_GID $file
	post=$(chk_get_version $CLIENT1 $file)
	if (($pre == $post)); then
		error "version not changed: pre $pre, post $post"
	fi
}
run_test 4b "setattr of GID changes versions"

test_4c() { # former test_0j
    local file=$DIR/$tfile
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT1 mcreate $file

    replay_barrier $SINGLEMDS
    do_node $CLIENT2 chgrp $RUNAS_GID $MOUNT2/$tfile
    do_node $CLIENT1 chown $RUNAS_ID:$RUNAS_GID $file
    zconf_umount $CLIENT2 $MOUNT2
    facet_failover $SINGLEMDS

    client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
    if ! do_node $CLIENT1 $CHECKSTAT -u \\\#$UID $file; then
		error_and_remount "setattr of UID succeeded unexpectedly"
    fi
}
run_test 4c "setattr of UID checks versions"

test_4d() { # former test_0k
    local file=$DIR/$tfile
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT1 mcreate $file

    replay_barrier $SINGLEMDS
    do_node $CLIENT2 chown $RUNAS_ID:$RUNAS_GID $MOUNT2/$tfile
    do_node $CLIENT1 chgrp $RUNAS_GID $file
    zconf_umount $CLIENT2 $MOUNT2
    facet_failover $SINGLEMDS

    client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
    if ! do_node $CLIENT1 $CHECKSTAT -g \\\#$UID $file; then
		error_and_remount "setattr of GID succeeded unexpectedly"
    fi
}
run_test 4d "setattr of GID checks versions"

test_4e() { # former test_0l
	local file=$DIR/$tfile
	local pre
	local post

	do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 chmod 666 $file
	post=$(chk_get_version $CLIENT1 $file)
	if (($pre == $post)); then
		error "version not changed: pre $pre, post $post"
	fi
}
run_test 4e "setattr of permission changes versions"

test_4f() { # former test_0m
    local file=$DIR/$tfile
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $file

    replay_barrier $SINGLEMDS
    do_node $CLIENT2 chgrp $RUNAS_GID $MOUNT2/$tfile
    do_node $CLIENT1 chmod 666 $file
    zconf_umount $CLIENT2 $MOUNT2
    facet_failover $SINGLEMDS

    client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
    if ! do_node $CLIENT1 $CHECKSTAT -p 0644 $file; then
		error_and_remount "setattr of permission succeeded unexpectedly"
    fi
}
run_test 4f "setattr of permission checks versions"

test_4g() { # former test_0n
	local file=$DIR/$tfile
	local pre
	local post

	do_node $CLIENT1 mcreate $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 chattr +i $file
	post=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 chattr -i $file
	if (($pre == $post)); then
		error "version not changed: pre $pre, post $post"
	fi
}
run_test 4g "setattr of flags changes versions"

checkattr() {
    local client=$1
    local attr=$2
    local file=$3
    local rc

    if ((${#attr} != 1)); then
        error "checking multiple attributes not implemented yet"
    fi
    do_node $client lsattr $file | cut -d ' ' -f 1 | grep -q $attr
}

test_4h() { # former test_0o
    local file=$DIR/$tfile
    local rc
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $file

    replay_barrier $SINGLEMDS
    do_node $CLIENT2 chmod 666 $MOUNT2/$tfile
    do_node $CLIENT1 chattr +i $file
    zconf_umount $CLIENT2 $MOUNT2
    facet_failover $SINGLEMDS

    client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
    checkattr $CLIENT1 i $file
    rc=$?
    do_node $CLIENT1 chattr -i $file
    if [ $rc -eq 0 ]; then
        error "setattr of flags succeeded unexpectedly"
    fi
}
run_test 4h "setattr of flags checks versions"

test_4i() { # former test_0p
	local file=$DIR/$tfile
	local pre
	local post
	local ad_orig
	local var=${SINGLEMDS}_svc

	ad_orig=$(do_facet $SINGLEMDS "$LCTL get_param mdd.${!var}.atime_diff")
	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.atime_diff=0"
	do_node $CLIENT1 mcreate $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 touch $file
	post=$(chk_get_version $CLIENT1 $file)
	#
	# We don't fail MDS in this test.  atime_diff shall be
	# restored to its original value.
	#
	do_facet $SINGLEMDS "$LCTL set_param $ad_orig"
	if (($pre != $post)); then
		error "version changed unexpectedly: pre $pre, post $post"
	fi
}
run_test 4i "setattr of times does not change versions"

test_4j() { # former test_0q
	local file=$DIR/$tfile
	local pre
	local post

	do_node $CLIENT1 mcreate $file
	pre=$(chk_get_version $CLIENT1 $file)
	do_node $CLIENT1 $TRUNCATE $file 1
	post=$(chk_get_version $CLIENT1 $file)
	if (($pre != $post)); then
		error "version changed unexpectedly: pre $pre, post $post"
	fi
}
run_test 4j "setattr of size does not change versions"

test_4k() { # former test_0r
    local file=$DIR/$tfile
    local mtime_pre
    local mtime_post
    local mtime
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
    do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.atime_diff=0"
    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $file

    replay_barrier $SINGLEMDS
    do_node $CLIENT2 chmod 666 $MOUNT2/$tfile
    do_node $CLIENT1 $TRUNCATE $file 1
    sleep 1
    mtime_pre=$(do_node $CLIENT1 stat --format=%Y $file)
    do_node $CLIENT1 touch $file
    sleep 1 # avoid stat caching
    mtime_post=$(do_node $CLIENT1 stat --format=%Y $file)
    zconf_umount $CLIENT2 $MOUNT2
    facet_failover $SINGLEMDS

    client_up $CLIENT1 || error "$CLIENT1 evicted"
    if (($mtime_pre >= $mtime_post)); then
        error "time not changed: pre $mtime_pre, post $mtime_post"
    fi
    if ! do_node $CLIENT1 $CHECKSTAT -s 1 $file; then
		error_and_remount "setattr of size failed"
    fi
    mtime=$(do_node $CLIENT1 stat --format=%Y $file)
    if (($mtime != $mtime_post)); then
        error "setattr of times failed: expected $mtime_post, got $mtime"
    fi
}
run_test 4k "setattr of times and size does not check versions"

test_5a() { # former test_0s
	local pre
	local post
	local tp_pre
	local tp_post

	mcreate $DIR/$tfile
	mkdir_on_mdt0 $DIR/$tdir
	pre=$(chk_get_version $CLIENT1 $DIR/$tfile)
	tp_pre=$(chk_get_version $CLIENT1 $DIR/$tdir)
	link $DIR/$tfile $DIR/$tdir/$tfile
	post=$(chk_get_version $CLIENT1 $DIR/$tfile)
	tp_post=$(chk_get_version $CLIENT1 $DIR/$tdir)
	if (($pre == $post)); then
		error "version of source not changed: pre $pre, post $post"
	fi
	if (($tp_pre != $tp_post)); then
		error "version of target parent was changed:"\
			"pre $tp_pre, post $tp_post"
	fi
}
run_test 5a "link changes versions of source but not target parent"

test_5b() { # former test_0t
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mcreate $DIR/$tfile
	mkdir_on_mdt0 $MOUNT/$tdir

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 777 $MOUNT2/$tdir
	link $DIR/$tfile $DIR/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if ! $CHECKSTAT -a $DIR/$tdir/$tfile; then
		error_and_remount "link should fail"
	fi
}
run_test 5b "link checks version of target parent"

test_5c() { # former test_0u
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	openfile -f O_RDWR:O_CREAT -m 0644 $DIR/$tfile
	mkdir_on_mdt0 $MOUNT/$tdir

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 666 $MOUNT2/$tfile
	link $DIR/$tfile $DIR/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if ! $CHECKSTAT -a $DIR/$tdir/$tfile; then
		error_and_remount "link should fail"
	fi
}
run_test 5c "link checks version of source"

test_6a() { # former test_0v
	local sp_pre
	local tp_pre
	local sp_post
	local tp_post

	mcreate $DIR/$tfile
	mkdir_on_mdt0 $MOUNT/$tdir
	sp_pre=$(chk_get_version $CLIENT1 $DIR)
	tp_pre=$(chk_get_version $CLIENT1 $DIR/$tdir)
	do_node $CLIENT1 mv $DIR/$tfile $DIR/$tdir/$tfile
	sp_post=$(chk_get_version $CLIENT1 $DIR)
	tp_post=$(chk_get_version $CLIENT1 $DIR/$tdir)
	if (($sp_pre != $sp_post)); then
		error "version of source parent was changed:" \
			"pre $sp_pre, post $sp_post"
	fi
	if (($tp_pre != $tp_post)); then
		error "version of target parent was changed:" \
			"pre $tp_pre, post $tp_post"
	fi
}
run_test 6a "rename doesn't change versions of source parent and target parent"

test_6b() { # former test_0w
	local pre
	local post

	do_node $CLIENT1 mcreate $DIR/$tfile
	pre=$(chk_get_version $CLIENT1 $DIR)
	do_node $CLIENT1 mv $DIR/$tfile $DIR/$tfile-new
	post=$(chk_get_version $CLIENT1 $DIR)
	if (($pre != $post)); then
		error "version of parent was changed: pre $pre, post $post"
	fi
}
run_test 6b "rename within same dir doesn't change version of parent"

test_6c() { # former test_0x
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mcreate $DIR/$tfile
	mkdir_on_mdt0 $MOUNT/$tdir

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 777 $MOUNT2
	mv $DIR/$tfile $DIR/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if $CHECKSTAT -a $DIR/$tfile; then
		error_and_remount "rename should fail"
	fi
}
run_test 6c "rename checks version of source parent"

test_6d() { # former test_0y
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mcreate $DIR/$tfile
	mkdir_on_mdt0 $MOUNT/$tdir

	replay_barrier $SINGLEMDS
	do_node $CLIENT2 chmod 777 $MOUNT2/$tdir
	mv $DIR/$tfile $DIR/$tdir/$tfile
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1 not evicted"
	if $CHECKSTAT -a $DIR/$tfile; then
		error_and_remount "rename should fail"
	fi
}
run_test 6d "rename checks version of target parent"

# pdirops tests, bug 18143
cycle=0
test_7_cycle() {
	local first=$1
	local lost=$2
	local last=$3
	local rc=0
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2
	cycle=$((cycle + 1))
	local cname=$TESTNAME.$cycle

	echo "start cycle: $cname"
	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mkdir_on_mdt0 $MOUNT/$tdir
	replay_barrier $SINGLEMDS
	# first operation
	echo "$cname first: $first"
	do_node $CLIENT1 $first || error "$cname: Cannot do first operation"
	# client2 operations that will be lost
	echo "$cname lost: $lost"
	do_node $CLIENT2 $lost || error "$cname: Cannot do 'lost' operations"
	# second operation
	echo "$cname last: $last"
	$last || error "$cname: Cannot do last operation"
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS
	# should fail as conflict expected
	client_evicted $CLIENT1 || rc=1

	wait_recovery_complete $SINGLEMDS
	wait_mds_ost_sync || error "wait_mds_ost_sync failed"

	rm -rf $DIR/$tdir
	return $rc
}

test_7a() {
	first="createmany -o $DIR/$tdir/$tfile- 1"
	lost="rm $MOUNT2/$tdir/$tfile-0"
	last="createmany -o $DIR/$tdir/$tfile- 1"
	test_7_cycle "$first" "$lost" "$last" || error "Test 7a.1 failed"

	first="createmany -o $DIR/$tdir/$tfile- 1"
	lost="rm $MOUNT2/$tdir/$tfile-0"
	last="mkdir $DIR/$tdir/$tfile-0"
	test_7_cycle "$first" "$lost" "$last" || error "Test 7a.2 failed"

	first="mkdir $DIR/$tdir/$tfile-0"
	lost="mv $MOUNT2/$tdir/$tfile-0 $MOUNT2/$tdir/$tfile-1"
	last="createmany -o $DIR/$tdir/$tfile- 1"
	test_7_cycle "$first" "$lost" "$last" || error "Test 7a.3 failed"
	return 0
}
run_test 7a "create, {lost}, create"

test_7b() {
    first="createmany -o $DIR/$tdir/$tfile- 1"
    lost="rm $MOUNT2/$tdir/$tfile-0; createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7b.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1"
    lost="touch $MOUNT2/$tdir/$tfile; mv $MOUNT2/$tdir/$tfile $MOUNT2/$tdir/$tfile-0"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7b.2 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1"
    lost="rm $MOUNT2/$tdir/$tfile-0; mkdir $MOUNT2/$tdir/$tfile-0"
    last="rmdir $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7b.3 failed"
    return 0
}
run_test 7b "create, {lost}, unlink"

test_7c() {
    first="createmany -o $DIR/$tdir/$tfile- 1"
    lost="rm $MOUNT2/$tdir/$tfile-0; createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7c.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2"
    lost="rm $MOUNT2/$tdir/$tfile-0; mkdir $MOUNT2/$tdir/$tfile-0"
    last="mv $DIR/$tdir/$tfile-1 $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7c.2 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1; mkdir $DIR/$tdir/$tfile-1-0"
    lost="rmdir $MOUNT2/$tdir/$tfile-1-0; createmany -o $MOUNT2/$tdir/$tfile-1- 1"
    last="mv $DIR/$tdir/$tfile-1-0 $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7c.3 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1"
    lost="mv $MOUNT2/$tdir/$tfile-0 $MOUNT2/$tdir/$tfile"
    last="mv $DIR/$tdir/$tfile $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7c.4 failed"
    return 0
}
run_test 7c "create, {lost}, rename"

test_7d() {
    first="createmany -o $DIR/$tdir/$tfile- 1; rm $DIR/$tdir/$tfile-0"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1; rm $MOUNT2/$tdir/$tfile-0"
    last="createmany -o $DIR/$tdir/$tfile- 1"
    test_7_cycle "$first" "$lost" "$last" && error "Test 7d.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1; rm $DIR/$tdir/$tfile-0"
    lost="mkdir $MOUNT2/$tdir/$tfile-0; rmdir $MOUNT2/$tdir/$tfile-0"
    last="mkdir $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" && error "Test 7d.2 failed"

    first="mkdir $DIR/$tdir/$tfile-0; rmdir $DIR/$tdir/$tfile-0"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1; mv $MOUNT2/$tdir/$tfile-0 $MOUNT2/$tdir/$tfile-1"
    last="createmany -o $DIR/$tdir/$tfile- 1"
    test_7_cycle "$first" "$lost" "$last" && error "Test 7d.3 failed"
    return 0
}
run_test 7d "unlink, {lost}, create"

test_7e() {
    first="createmany -o $DIR/$tdir/$tfile- 1; rm $DIR/$tdir/$tfile-0"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1; rm $MOUNT2/$tdir/$tfile-0;createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7e.1 failed"

    first="mkdir $DIR/$tdir/$tfile-0; rmdir $DIR/$tdir/$tfile-0"
    lost="mkdir $MOUNT2/$tdir/$tfile-0; rmdir $MOUNT2/$tdir/$tfile-0; mkdir $MOUNT2/$tdir/$tfile-0"
    last="rmdir $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7e.2 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1; rm $DIR/$tdir/$tfile-0"
    lost="mkdir $MOUNT2/$tdir/$tfile-0"
    last="rmdir $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7e.3 failed"

    first="mkdir $DIR/$tdir/$tfile-0; rmdir $DIR/$tdir/$tfile-0"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7e.4 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2; rm $DIR/$tdir/$tfile-0"
    lost="mv $MOUNT2/$tdir/$tfile-1 $MOUNT2/$tdir/$tfile-0"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7e.5 failed"
    return 0
}
run_test 7e "unlink, {lost}, unlink"

test_7f() {
    first="createmany -o $DIR/$tdir/$tfile- 1; rm $DIR/$tdir/$tfile-0"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7f.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2; rm $DIR/$tdir/$tfile-0"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="mv $DIR/$tdir/$tfile-1 $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7f.2 failed"

    first="mkdir $DIR/$tdir/$tfile; createmany -o $DIR/$tdir/$tfile- 1; rmdir $DIR/$tdir/$tfile"
    lost="mkdir $MOUNT2/$tdir/$tfile"
    last="mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7f.3 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2; rm $DIR/$tdir/$tfile-0"
    lost="mv $MOUNT2/$tdir/$tfile-1 $MOUNT2/$tdir/$tfile-0"
    last="mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7f.4 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2; rm $DIR/$tdir/$tfile-0"
    lost="mkdir $MOUNT2/$tdir/$tfile-0"
    last="mv $DIR/$tdir/$tfile-1 $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7f.5 failed"
    return 0
}
run_test 7f "unlink, {lost}, rename"

test_7g() {
	first="createmany -o $DIR/$tdir/$tfile- 1; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
	lost="mkdir $MOUNT2/$tdir/$tfile-0;rmdir $MOUNT2/$tdir/$tfile-0"
	last="createmany -o $DIR/$tdir/$tfile- 1"
	test_7_cycle "$first" "$lost" "$last" && error "Test 7g.1 failed"

	first="createmany -o $DIR/$tdir/$tfile- 2; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
	lost="createmany -o $MOUNT2/$tdir/$tfile- 1; rm $MOUNT2/$tdir/$tfile-0"
	last="mkdir $DIR/$tdir/$tfile-0"
	test_7_cycle "$first" "$lost" "$last" && error "Test 7g.2 failed"

	first="createmany -o $DIR/$tdir/$tfile- 1; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile"
	lost="createmany -o $MOUNT2/$tdir/$tfile- 1"
	last="link $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
	if [ "$MDS1_VERSION" -lt $(version_code 2.5.1) ]; then
		test_7_cycle "$first" "$lost" "$last" ||
			error "Test 7g.3 failed"
	else #LU-4442 LU-3528
		test_7_cycle "$first" "$lost" "$last" &&
			error "Test 7g.3 failed"
	fi
    return 0
}
run_test 7g "rename, {lost}, create"

test_7h() {
    first="createmany -o $DIR/$tdir/$tfile- 1; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7h.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2; mv $DIR/$tdir/$tfile-1 $DIR/$tdir/$tfile-0"
    lost="rm $MOUNT2/$tdir/$tfile-0; createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="rm $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7h.2 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1; mkdir $DIR/$tdir/$tfile; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile"
    lost="rm $MOUNT2/$tdir/$tfile/$tfile-0"
    last="rmdir $DIR/$tdir/$tfile"
    #test_7_cycle "$first" "$lost" "$last" || error "Test 7h.3 failed"
    return 0
}
run_test 7h "rename, {lost}, unlink"

test_7i() {
    first="createmany -o $DIR/$tdir/$tfile- 1; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
    lost="createmany -o $MOUNT2/$tdir/$tfile- 1"
    last="mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7i.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 1; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-1"
    lost="mkdir $MOUNT2/$tdir/$tfile-0"
    last="mv $DIR/$tdir/$tfile-1 $DIR/$tdir/$tfile-0"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7i.1 failed"

    first="createmany -o $DIR/$tdir/$tfile- 3; mv $DIR/$tdir/$tfile-1 $DIR/$tdir/$tfile-0"
    lost="mv $MOUNT2/$tdir/$tfile-2 $MOUNT2/$tdir/$tfile-0"
    last="mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile-2"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7i.3 failed"

    first="createmany -o $DIR/$tdir/$tfile- 2; mv $DIR/$tdir/$tfile-0 $DIR/$tdir/$tfile"
    lost="rm $MOUNT2/$tdir/$tfile-1"
    last="mv $DIR/$tdir/$tfile $DIR/$tdir/$tfile-1"
    test_7_cycle "$first" "$lost" "$last" || error "Test 7i.4 failed"
    return 0
}
run_test 7i "rename, {lost}, rename"

# test set #8: orphan handling bug 15392.
# Unlink during recovery creates orphan always just in case some late open may
# arrive. These orphans will be removed after recovery anyway.
# Tests check that valid create,unlink,create sequence will work in this case
# too but not fail on second create due to orphan found.

test_8a() {
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	mcreate $DIR/$tfile
	mkdir_on_mdt0 $DIR/$tfile-2
	replay_barrier $SINGLEMDS
	# missed replay from client2 will lead to recovery by versions
	do_node $CLIENT2 touch $MOUNT2/$tfile-2/$tfile
	rm $DIR/$tfile || return 1
	touch $DIR/$tfile || return 2

	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS
	client_up $CLIENT1 || return 6

	rm $DIR/$tfile || error "$tfile doesn't exists"
	rm -rf $DIR/$tfile-2
	return 0
}
run_test 8a "create | unlink, create shouldn't fail"

test_8b() {
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	do_node $CLIENT1 touch $DIR/$tfile
	mkdir_on_mdt0 $DIR/$tfile-2
	replay_barrier $SINGLEMDS
	# missed replay from client2 will lead to recovery by versions
	do_node $CLIENT2 touch $MOUNT2/$tfile-2/$tfile
	rm -f $MOUNT1/$tfile || return 1
	mcreate $MOUNT1/$tfile || return 2

	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS
	client_up $CLIENT1 || return 6

	rm $MOUNT1/$tfile || error "$tfile doesn't exists"
	rm -rf $DIR/$tfile-2
	return 0
}
run_test 8b "create | unlink, create shouldn't fail"

test_8c() {
	local var=${SINGLEMDS}_svc
	zconf_mount $CLIENT2 $MOUNT2

	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	touch $DIR/$tfile
	mkdir_on_mdt0 $DIR/$tfile-2
	replay_barrier $SINGLEMDS
	# missed replay from client2 will lead to recovery by versions
	do_node $CLIENT2 touch $MOUNT2/$tfile-2/$tfile
	rm -f $MOUNT1/$tfile || return 1
	mkdir_on_mdt0 $MOUNT1/$tfile || return 2

    	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS
	client_up $CLIENT1 || return 6

	rmdir $MOUNT1/$tfile || error "$tfile doesn't exists"
	rm -rf $MOUNT1/$tfile-2
	return 0
}
run_test 8c "create | unlink, create shouldn't fail"

#
# This test uses three Lustre clients on two hosts.
#
#   Lustre Client 1:    $CLIENT1:$MOUNT     ($DIR)
#   Lustre Client 2:    $CLIENT2:$MOUNT2    ($DIR2)
#   Lustre Client 3:    $CLIENT2:$MOUNT1    ($DIR1)
#
test_10b() { # former test_2b
	local pre
	local post
	local var=${SINGLEMDS}_svc

	[ $CLIENTCOUNT -ge 2 ] || \
		{ skip "Need two or more clients, have $CLIENTCOUNT" && \
			exit 0; }

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	zconf_mount $CLIENT1 $MOUNT
	zconf_mount $CLIENT2 $MOUNT1
	zconf_mount $CLIENT2 $MOUNT2
	do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $DIR/$tfile-a
	do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $DIR/$tfile-b

	#
	# Save an MDT transaction number before recovery.
	#
	do_node $CLIENT1 touch $DIR1/$tfile
	pre=$(chk_get_version $CLIENT1 $DIR/$tfile)

	#
	# Comments on the replay sequence state the expected result
	# of each request.
	#
	#   "R"     Replayed.
	#   "U"     Unable to replay.
	#   "J"     Rejected.
	#
	replay_barrier $SINGLEMDS
	do_node $CLIENT1 chmod 666 $DIR/$tfile-a            # R
	do_node $CLIENT2 chmod 666 $DIR1/$tfile-b           # R
	do_node $CLIENT2 chgrp $RUNAS_GID $DIR2/$tfile-a    # U
	do_node $CLIENT1 chown $RUNAS_ID:$RUNAS_GID $DIR/$tfile-a      # J
	do_node $CLIENT2 $TRUNCATE $DIR2/$tfile-b 1          # U
	do_node $CLIENT2 chgrp $RUNAS_GID $DIR1/$tfile-b    # R
	do_node $CLIENT1 chown $RUNAS_ID:$RUNAS_GID $DIR/$tfile-b      # R
	zconf_umount $CLIENT2 $MOUNT2
	facet_failover $SINGLEMDS

	client_evicted $CLIENT1 || error "$CLIENT1:$MOUNT not evicted"
	client_up $CLIENT2 || error "$CLIENT2:$MOUNT1 evicted"

	#
	# Check the MDT epoch.  $post must be the first transaction
	# number assigned after recovery.
	#
	do_node $CLIENT2 chmod 666 $DIR1/$tfile
	post=$(chk_get_version $CLIENT2 $DIR1/$tfile)
	if (($(($pre >> 32)) == $((post >> 32)))); then
		error "epoch not changed: pre $pre, post $post"
	fi

	if (($(($post & 0x00000000ffffffff)) != 1)); then
		error "transno should restart from one: got $post"
	fi

	do_node $CLIENT2 stat $DIR1/$tfile-a
	do_node $CLIENT2 stat $DIR1/$tfile-b

	do_node $CLIENT2 $CHECKSTAT -p 0666 -u \\\#$UID -g \\\#$UID \
		$DIR1/$tfile-a || error "$DIR/$tfile-a: unexpected state"
	do_node $CLIENT2 $CHECKSTAT -p 0666 -u \\\#$RUNAS_ID -g \\\#$RUNAS_GID \
		$DIR1/$tfile-b || error "$DIR/$tfile-b: unexpected state"

	zconf_umount $CLIENT2 $MOUNT1
}
run_test 10b "3 clients: some, none, and all reqs replayed"

# test set #11: operations in single directory
test_11a() {
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    replay_barrier $SINGLEMDS

    do_node $CLIENT1 createmany -o $DIR/$tfile-1- 100 &
    PID=$!
    do_node $CLIENT2 createmany -o $MOUNT2/$tfile-2- 100
    zconf_umount $CLIENT2 $MOUNT2
    wait $PID

    facet_failover $SINGLEMDS
    # recovery shouldn't fail due to missing client 2
    client_up $CLIENT1 || return 1
    # All files from client1 should have been replayed
    do_node $CLIENT1 unlinkmany $DIR/$tfile-1- 100 || return 2

    [ -e $DIR/$tdir/$tfile-2-0 ] && error "$tfile-2-0 exists"
    return 0
}
run_test 11a "concurrent creates don't affect each other"

test_11b() {
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT2 createmany -o $MOUNT2/$tfile-2- 100

    replay_barrier $SINGLEMDS
    do_node $CLIENT1 createmany -o $DIR/$tfile-1- 100 &
    PID=$!
    do_node $CLIENT2 unlinkmany -o $MOUNT2/$tfile-2- 100
    zconf_umount $CLIENT2 $MOUNT2
    wait $PID

    facet_failover $SINGLEMDS
    # recovery shouldn't fail due to missing client 2
    client_up $CLIENT1 || return 1
    # All files from client1 should have been replayed
    do_node $CLIENT1 unlinkmany $DIR/$tfile-1- 100 || return 2

    [ -e $DIR/$tdir/$tfile-2-0 ] && error "$tfile-2-0 exists"
    return 0
}
run_test 11b "concurrent creates and unlinks don't affect each other"

# test set #12: lock replay with VBR, bug 16356
test_12a() { # former test_2a
    local var=${SINGLEMDS}_svc
    zconf_mount $CLIENT2 $MOUNT2

    do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

    do_node $CLIENT2 $LFS mkdir -i0 -c1 $MOUNT2/$tdir
    replay_barrier $SINGLEMDS
    do_node $CLIENT2 mcreate $MOUNT2/$tdir/$tfile
    do_node $CLIENT1 createmany -o $DIR/$tfile- 25
    #client1 read data from client2 which will be lost
    do_node $CLIENT1 $CHECKSTAT $DIR/$tdir/$tfile
    do_node $CLIENT1 createmany -o $DIR/$tfile-3- 25
    zconf_umount $CLIENT2 $MOUNT2

    facet_failover $SINGLEMDS
    # recovery shouldn't fail due to missing client 2
    client_up $CLIENT1 || return 1

    # All 50 files should have been replayed
    do_node $CLIENT1 unlinkmany $DIR/$tfile- 25 || return 2
    do_node $CLIENT1 unlinkmany $DIR/$tfile-3- 25 || return 3
    do_node $CLIENT1 $CHECKSTAT $DIR/$tdir/$tfile && return 4

    return 0
}
run_test 12a "lost data due to missed REMOTE client during replay"

test_13() { # LU-8826
	local var=${SINGLEMDS}_svc

	if combined_mgs_mds ; then
		skip "Needs separate MGS to enable IR"
		return 0
	fi

	do_facet $SINGLEMDS "$LCTL set_param mdd.${!var}.sync_permission=0"
	do_facet $SINGLEMDS "$LCTL set_param mdt.${!var}.commit_on_sharing=0"

	zconf_mount $CLIENT2 $MOUNT2
	do_node $CLIENT1 openfile -f O_RDWR:O_CREAT -m 0644 $DIR/$tfile

	# set ir_timeout to a reasonable small value
	local ir_timeout=$(do_facet mgs $LCTL get_param -n mgs.*.ir_timeout)
	do_facet mgs $LCTL set_param mgs.*.ir_timeout=5
	# make sure IR functional
	sleep 5

	replay_barrier $SINGLEMDS
	do_node $CLIENT1 chmod 666 $DIR/$tfile
	do_node $CLIENT2 chmod 777 $DIR2/$tfile

	# make sure client data of $CLIENT2:$MOUNT2 is remained
	# define OBD_FAIL_TGT_CLIENT_DEL	0x718
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x718
	zconf_umount $CLIENT2 $MOUNT2
	# define OBD_FAIL_TGT_SLUGGISH_NET	0x719
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x719
	facet_failover $SINGLEMDS

	client_up $CLIENT1 || error "$CLIENT1 evicted"

	do_facet $SINGLEMDS $LCTL set_param fail_loc=0
	do_facet mgs $LCTL set_param mgs.*.ir_timeout=$ir_timeout

	do_node $CLIENT1 $CHECKSTAT -p 0666 $DIR/$tfile ||
		error "$DIR/$tfile-a: unexpected state"
}
run_test 13 "Shouldn't give up VBR easily on sluggish network"

#restore COS setting
restore_lustre_params < $cos_param_file
rm -f $cos_param_file

[ "$CLIENTS" ] && zconf_mount_clients $CLIENTS $DIR

complete_test $SECONDS
check_and_cleanup_lustre
exit_status
