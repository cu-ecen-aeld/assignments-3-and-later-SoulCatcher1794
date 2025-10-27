#!/bin/bash

filesdir=$1
searchstr=$2

if [ -z "$filesdir" ] || [ -z "$searchstr" ]; then
    echo "Either the directory or the string arguments were not specified"
    exit 1
elif [ ! -d "$filesdir" ]; then
    echo "${filesdir} is not a directory"
    exit 1
else
    line_count=$(grep -r "$searchstr" "$filesdir" | wc -l)
    files_count=$(grep -r -l "$searchstr" "$filesdir" | wc -l)
    echo "The number of files are ${files_count} and the number of matching lines are ${line_count}"
fi