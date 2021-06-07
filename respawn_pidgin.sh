#!/bin/env bash
# Test if fuzzing dbus didn't kill pidgin eventually

outdir="./fuzz2"
outfile='address_sanitizer.log'

while true;
do
	if [ "`pidof pidgin`" == '' ]
	then
		./pidgin-2.14.5/pidgin/pidgin >> ${outdir}/${outfile} &
	fi
done


