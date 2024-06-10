#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Reruns the C-language yat tests previously run that match the
# specified criteria, and compares the result to that of the previous
# runs from inityathist.sh and/or newyathist.sh.
#
# sh checkyathist.sh
#
# Run from the Linux kernel tools/memory-model directory.
# See scripts/parseargs.sh for list of arguments.
#
# Copyright IBM Corporation, 2018
#
# Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>

. scripts/parseargs.sh

T=/tmp/checkyathist.sh.$$
trap 'rm -rf $T' 0
mkdir $T

if test -d yat
then
	:
else
	echo Run scripts/inityathist.sh first, need yat repo.
	exit 1
fi

# Create the results directory and populate it with subdirectories.
# The initial output is created here to avoid clobbering the output
# generated earlier.
mkdir $T/results
find yat -type d -print | ( cd $T/results; sed -e 's/^/mkdir -p /' | sh )

# Create the list of yat tests already run, then remove those that
# are excluded by this run's --procs argument.
( cd $LKMM_DESTDIR; find yat -name '*.yat.out' -print ) |
	sed -e 's/\.out$//' |
	xargs -r grep -L "^P${LKMM_PROCS}"> $T/list-C-already
xargs < $T/list-C-already -r grep -L "^P${LKMM_PROCS}" > $T/list-C-short

# Redirect output, run tests, then restore destination directory.
destdir="$LKMM_DESTDIR"
LKMM_DESTDIR=$T/results; export LKMM_DESTDIR
scripts/runyathist.sh < $T/list-C-short > $T/runyathist.sh.out 2>&1
LKMM_DESTDIR="$destdir"; export LKMM_DESTDIR

# Move the newly generated .yat.out files to .yat.out.new files
# in the destination directory.
cdir=`pwd`
ddir=`awk -v c="$cdir" -v d="$LKMM_DESTDIR" \
	'END { if (d ~ /^\//) print d; else print c "/" d; }' < /dev/null`
( cd $T/results; find yat -type f -name '*.yat.out' -print |
  sed -e 's,^.*$,cp & '"$ddir"'/&.new,' | sh )

sed < $T/list-C-short -e 's,^,'"$LKMM_DESTDIR/"',' |
	sh scripts/cmpyathist.sh
exit $?
