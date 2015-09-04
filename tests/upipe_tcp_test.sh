#!/bin/sh

if [ $# -ne 1 ]; then
    exit 1
fi

srcdir="$1"
UNAME=$(uname)
TESTNAME=upipe_tcp_test
TMP="`mktemp -d tmp.XXXXXXXXXX`"
FILE="$srcdir"/Makefile.am

if ! "$srcdir"/"$TESTNAME" $FILE "$TMP"/output > /dev/null 2> /dev/null; then
    rm -rf "$TMP"
    exit 1
fi

if ! diff "$FILE" "$TMP"/output; then
    rm -rf "$TMP"
    exit 1
fi

rm -f "$TMP"/output

if ! which valgrind >/dev/null 2>&1; then
	echo "#### Please install valgrind for unit tests"
	rm -rf "$TMP"
	exit 1
fi

# valgrind suppressions
VALGRIND_SUPPRESSIONS=" --suppressions=$srcdir/valgrind.supp "
if [ "$UNAME" = "Darwin" ]; then
    VALGRIND_SUPPRESSIONS+=" --suppressions=$srcdir/valgrind_osx.supp "
fi

# Run in valgrind, with leak checking enabled
libtool --mode=execute valgrind -q --leak-check=full $VALGRIND_SUPPRESSIONS "$srcdir"/"$TESTNAME" "$FILE" "$TMP"/output > /dev/null 2> "$TMP"/logs
RET=$?
if test -s "$TMP"/logs; then
    cat "$TMP"/logs >&2
    rm -rf "$TMP"
    exit 1
fi

rm -rf "$TMP"
