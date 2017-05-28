#!/bin/bash -e

BIN=.gg/exe/bin
PATH=`pwd`/$BIN:$PATH

# Download binaries
mkdir -p $BIN
if [ ! -f $BIN/cc1 ]; then
  wget  https://s3-us-west-2.amazonaws.com/gg-us-west-2/bin/cc1 -O $BIN/cc1
  chmod u+x $BIN/cc1
fi

if [ ! -f $BIN/x86_64-linux-musl-gcc ]; then
  wget  https://s3-us-west-2.amazonaws.com/gg-us-west-2/bin/x86_64-linux-musl-gcc -O $BIN/x86_64-linux-musl-gcc
  chmod u+x $BIN/x86_64-linux-musl-gcc
fi