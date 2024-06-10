#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Runs the C-language yat tests matching the specified criteria
# that do not already have a corresponding .yat.out file, and does
# not judge the result.
#
# sh newyathist.sh
#
# Run from the Linux kernel tools/memory-model directory.
# See scripts/parseargs.sh for list of arguments.
#
# Copyright IBM Corporation, 2018
#
# Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>

. scripts/parseargs.sh

T=/tmp/newyathist.sh.$$
trap 'rm -rf $T' 0
mkdir $T

if test -d yat
then
	:
else
	echo Run scripts/inityathist.sh first, need yat repo.
	exit 1
fi

# Create any new directories that have appeared in the github yat
# repo since the last run.
if test "$LKMM_DESTDIR" != "."
then
	find yat -type d -print |
	( cd "$LKMM_DESTDIR"; sed -e 's/^/mkdir -p /' | sh )
fi

# Create a list of the C-language yat tests previously run.
( cd $LKMM_DESTDIR; find yat -name '*.yat.out' -print ) |
	sed -e 's/\.out$//' |
	xargs -r grep -L "^P${LKMM_PROCS}"> $T/list-C-already

# Form full list of yat tests with no more than the specified
# number of processes (per the --procs argument).
find yat -name '*.yat' -exec grep -l -m 1 "^C " {} \; > $T/list-C-all
xargs < $T/list-C-all -r grep -L "^P${LKMM_PROCS}" > $T/list-C-short

# Form list of new tests.  Note: This does not handle yat-test deletion!
sort $T/list-C-already $T/list-C-short | uniq -u > $T/list-C-new

# Form list of yat tests that have changed since the last run.
sed < $T/list-C-short -e 's,^.*$,if test & -nt '"$LKMM_DESTDIR"'/&.out; then echo &; fi,' > $T/list-C-script
sh $T/list-C-script > $T/list-C-newer

# Merge the list of new and of updated yat tests: These must be (re)run.
sort -u $T/list-C-new $T/list-C-newer > $T/list-C-needed

scripts/runyathist.sh < $T/list-C-needed

exit 0
