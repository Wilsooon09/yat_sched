#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Run herd7 tests on all .yat files in the yat-tests directory
# and check each file's result against a "Result:" comment within that
# yat test.  If the verification result does not match that specified
# in the yat test, this script prints an error message prefixed with
# "^^^".  It also outputs verification results to a file whose name is
# that of the specified yat test, but with ".out" appended.
#
# Usage:
#	checkallyat.sh
#
# Run this in the directory containing the memory model.
#
# This script makes no attempt to run the yat tests concurrently.
#
# Copyright IBM Corporation, 2018
#
# Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>

. scripts/parseargs.sh

yatdir=yat-tests
if test -d "$yatdir" -a -r "$yatdir" -a -x "$yatdir"
then
	:
else
	echo ' --- ' error: $yatdir is not an accessible directory
	exit 255
fi

# Create any new directories that have appeared in the github yat
# repo since the last run.
if test "$LKMM_DESTDIR" != "."
then
	find $yatdir -type d -print |
	( cd "$LKMM_DESTDIR"; sed -e 's/^/mkdir -p /' | sh )
fi

# Find the checkyat script.  If it is not where we expect it, then
# assume that the caller has the PATH environment variable set
# appropriately.
if test -x scripts/checkyat.sh
then
	clscript=scripts/checkyat.sh
else
	clscript=checkyat.sh
fi

# Run the script on all the yat tests in the specified directory
ret=0
for i in $yatdir/*.yat
do
	if ! $clscript $i
	then
		ret=1
	fi
done
if test "$ret" -ne 0
then
	echo " ^^^ VERIFICATION MISMATCHES" 1>&2
else
	echo All yat tests verified as was expected. 1>&2
fi
exit $ret
