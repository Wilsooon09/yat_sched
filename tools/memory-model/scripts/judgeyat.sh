#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Given a .yat test and the corresponding .yat.out file, check
# the .yat.out file against the "Result:" comment to judge whether
# the test ran correctly.
#
# Usage:
#	judgeyat.sh file.yat
#
# Run this in the directory containing the memory model, specifying the
# pathname of the yat test to check.
#
# Copyright IBM Corporation, 2018
#
# Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>

yat=$1

if test -f "$yat" -a -r "$yat"
then
	:
else
	echo ' --- ' error: \"$yat\" is not a readable file
	exit 255
fi
if test -f "$LKMM_DESTDIR/$yat".out -a -r "$LKMM_DESTDIR/$yat".out
then
	:
else
	echo ' --- ' error: \"$LKMM_DESTDIR/$yat\".out is not a readable file
	exit 255
fi
if grep -q '^ \* Result: ' $yat
then
	outcome=`grep -m 1 '^ \* Result: ' $yat | awk '{ print $3 }'`
else
	outcome=specified
fi

grep '^Observation' $LKMM_DESTDIR/$yat.out
if grep -q '^Observation' $LKMM_DESTDIR/$yat.out
then
	:
else
	echo ' !!! Verification error' $yat
	if ! grep -q '!!!' $LKMM_DESTDIR/$yat.out
	then
		echo ' !!! Verification error' >> $LKMM_DESTDIR/$yat.out 2>&1
	fi
	exit 255
fi
if test "$outcome" = DEADLOCK
then
	if grep '^Observation' $LKMM_DESTDIR/$yat.out | grep -q 'Never 0 0$'
	then
		ret=0
	else
		echo " !!! Unexpected non-$outcome verification" $yat
		if ! grep -q '!!!' $LKMM_DESTDIR/$yat.out
		then
			echo " !!! Unexpected non-$outcome verification" >> $LKMM_DESTDIR/$yat.out 2>&1
		fi
		ret=1
	fi
elif grep '^Observation' $LKMM_DESTDIR/$yat.out | grep -q $outcome || test "$outcome" = Maybe
then
	ret=0
else
	echo " !!! Unexpected non-$outcome verification" $yat
	if ! grep -q '!!!' $LKMM_DESTDIR/$yat.out
	then
		echo " !!! Unexpected non-$outcome verification" >> $LKMM_DESTDIR/$yat.out 2>&1
	fi
	ret=1
fi
tail -2 $LKMM_DESTDIR/$yat.out | head -1
exit $ret
