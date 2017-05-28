#!/bin/bash -e

BIN=.gg/exe/bin
PATH=`pwd`/$BIN:$PATH
MAGIC="##GGTHUNK##"


# Run with GCC
$BIN/x86_64-linux-musl-gcc -g -O2 -c -S -frandom-seed=winstein -o remake.s.gold $DATADIR/examples/remake.i

# Create thunk
GG_DIR=.gg/ python $DATADIR/../models/gg-model-compile.py -g -O2 -c -S -frandom-seed=winstein -o remake-python.s $DATADIR/examples/remake.i


# Assert it's a thunk
if [ $MAGIC != `head -c 11 remake-python.s` ]; then
  echo "Failed to create thunk"
  exit 1
fi


# execute thunk
../frontend/gg-execute remake-python.s


# check difference
diff remake-python.s remake.s.gold


# Clean up
# Note: Not cleaning up .gg directory
rm -f remake.s.gold
rm -f remake-python.s
find .gg -maxdepth 1 -type f -delete

exit 0
