#!/bin/bash
#
# Test various combinations of file size, number of connections,
# and I/O buffer size with uring_svr. All tests basically just
# download files from the test/data directory, then verify the
# size and shasum to make sure they came through intact.

# Build a bucket for downloaded files.
mkdir -p ./bucket

# Start ../src/uring_svr and tell it to serve a specific
# number of files, then exit.
function start_uring_svr() {
	echo Starting uring_svr...
	io_buf_sz=$1
	n_files=$2
	nohup ../src/uring_svr 12345 -vvvvvvv -b${io_buf_sz} -n${n_files} > test.log 2>&1 &
	URING_SVR_PID=$!
	echo uring_svr PID is $URING_SVR_PID.
	sleep 3
}

function fetch_files() {
	UR_CONNECTIONS=$1
	UR_FNAME=$2
	echo Fetching $UR_CONNECTIONS copies of $UR_FNAME...
	rm -f bucket/*
	x=0
	while [ $x -lt $UR_CONNECTIONS ] ; do
		( echo $UR_FNAME | nc 127.0.0.1 12345 > bucket/$(basename $UR_FNAME)_$x & ) ;
		x=$((x+1)) ;
	done
}

function verify_files() {
	UR_CONNECTIONS=$1
	UR_FNAME=$2
	echo Verifying  $UR_CONNECTIONS copies of $UR_FNAME...

	real_sha=$(shasum $UR_FNAME | cut -d' ' -f1)
	x=0
	while [ $x -lt $UR_CONNECTIONS ] ; do
		fetch_sha=$(shasum bucket/$(basename $UR_FNAME)_$x | cut -d' ' -f1)
	if [ ! "$fetch_sha" == "$real_sha" ] ; then
			echo "SHASUM mismatch on $UR_FNAME, expected $real_sha got $fetch_sha"
		fi
		x=$((x+1)) ;
	done
}

echo Test case 1.
start_uring_svr 16K 1
fetch_files 1 data/nums.txt
wait $URING_SVR_PID
verify_files 1 data/nums.txt
rm -f bucket/*
echo

echo Test case 2,
start_uring_svr 1024K 20
fetch_files 20 data/nums.txt
wait $URING_SVR_PID
verify_files 20 data/nums.txt
rm -f bucket/*
echo

echo Test case 3.
start_uring_svr 1024K 1
fetch_files 1 data/r3.dat
wait $URING_SVR_PID
verify_files 1 data/r3.dat
rm -f bucket/*
echo

echo Test case 4: transfer 120 copies of tiny.dat.
start_uring_svr 1024K 120
fetch_files 120 data/tiny.dat
wait $URING_SVR_PID
verify_files 120 data/tiny.dat
rm bucket/*
echo

echo Big test case - generate a 1GB file and transfer it.
dd if=/dev/urandom of=data/1G.dat bs=1024 count=$(expr 1024 \* 1024)
start_uring_svr 1024K 2
fetch_files 2 data/1G.dat
wait $URING_SVR_PID
verify_files 2 data/1G.dat
rm -f bucket/*
echo

echo Append a bit onto 1G.dat and fetch again.
echo 0123456789 >> data/1G.dat
start_uring_svr 16M 1
fetch_files 1 data/1G.dat
wait $URING_SVR_PID
verify_files 1 data/1G.dat
rm -f bucket/*

echo Deleting 1G.dat...
rm -f data/1G.dat
echo Done.

