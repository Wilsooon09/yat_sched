#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Run a herd7 test and invokes judgeyat.sh to check the result against
# a "Result:" comment within the yat test.  It also outputs verification
# results to a file whose name is that of the specified yat test, but
# with ".out" appended.
#
# Usage:
#	checkyat.sh file.yat
#
# Run this in the directory containing the memory model, specifying the
# pathname of the yat test to check.  The caller is expected to have
# properly set up the LKMM environment variables.
#
# Copyright IBM Corporation, 2018
#
# Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>

yat=$1
herdoptions=${LKMM_HERD_OPTIONS--conf linux-kernel.cfg}

if test -f "$yat" -a -r "$yat"
then
	:
else
	echo ' --- ' error: \"$yat\" is not a readable file
	exit 255
fi

echo Herd options: $herdoptions > $LKMM_DESTDIR/$yat.out
/usr/bin/time $LKMM_TIMEOUT_CMD herd7 $herdoptions $yat >> $LKMM_DESTDIR/$yat.out 2>&1

scripts/judgeyat.sh $yat
