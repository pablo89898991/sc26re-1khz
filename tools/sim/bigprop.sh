#!/bin/bash
cd /home/claude/fw/sc26re/tools/sim
export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so) ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so
python3 prop.py 1 600 > /tmp/bp_prop.log 2>&1
python3 prop2.py 1 600 > /tmp/bp_prop2.log 2>&1
python3 protofuzz.py 61 260 > /tmp/bp_proto.log 2>&1
echo done > /tmp/bp_done
