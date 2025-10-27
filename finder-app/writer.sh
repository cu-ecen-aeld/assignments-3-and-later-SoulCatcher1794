#!/bin/bash

writefile=$1
writestr=$2

if [ -z "$writefile" ] || [ -z "$writestr" ]; then
    echo "Either the directory or the string arguments were not specified"
    exit 1
fi

mkdir -p "$(dirname "$writefile")"

echo "$writestr" > "$writefile" || { 
    echo "Failed to create file";
    exit 1;
}

