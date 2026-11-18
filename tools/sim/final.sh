#!/bin/bash
cd /home/claude/fw/sc26re/tools/sim
export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so) ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so
( for s in $(seq 1 10); do python3 fuzz.py $s 40 2>&1 | tail -3; done > /tmp/r_fuzz.log; ./rounds.sh 0 1 10 > /tmp/r_usb.log 2>&1; ./rounds.sh 2 1 20 > /tmp/r_bt.log 2>&1 ) &
./rounds.sh 1 1 20 > /tmp/r_puck.log 2>&1
wait
echo done > /tmp/r_done
