#!/bin/bash
# usage: rounds.sh MODE FIRST LAST  (MODE 0 USB, 1 Puck, 2 BT)
export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so) ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so
for s in $(seq $2 $3); do
  python3 wfuzz.py $s $1 x 30 2>&1 | tail -4
  if [ "$1" != 0 ]; then python3 wfuzz.py $s $1 d 15 2>&1 | tail -4; fi
done
