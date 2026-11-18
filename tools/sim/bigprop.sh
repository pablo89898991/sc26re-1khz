#!/bin/bash
cd /home/claude/fw/sc26re/tools/sim
export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so) ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so
A=${1:-1}; B=${2:-600}
python3 prop.py $A $B > /tmp/bp_prop.log 2>&1
python3 prop2.py $A $B > /tmp/bp_prop2.log 2>&1
python3 protofuzz.py $A $((A + 150)) > /tmp/bp_proto.log 2>&1
SIMLIB=libsim_gm_asan.so python3 test_gyro.py > /tmp/bp_gyro.log 2>&1
echo done > /tmp/bp_done
