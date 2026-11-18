#!/bin/bash
# endless: new seeds every round, all transports, ASan/UBSan. Problems go to /tmp/soak_bad.log
cd /home/claude/fw/sc26re/tools/sim
export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so) ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so
s=${1:-100}
while true; do
  for job in "GM wfuzz.py $s 0 x 12" "GM wfuzz.py $s 1 x 12" "GM wfuzz.py $s 2 d 8" "fuzz.py $s 25" "wfuzz.py $s 0 x 20" "wfuzz.py $s 1 x 20" "wfuzz.py $s 1 d 10" "wfuzz.py $s 2 x 20" "wfuzz.py $s 2 d 10"; do
    if [ "${job:0:3}" = "GM " ]; then out=$(SIMLIB=libsim_gm_asan.so timeout 900 python3 ${job:3} 2>&1); else out=$(timeout 900 python3 $job 2>&1); fi
    last=$(echo "$out" | tail -1)
    echo "$(date +%T) $job :: $last" >> /tmp/soak.log
    if ! echo "$last" | grep -q ': 0 problems'; then echo "== $job" >> /tmp/soak_bad.log; echo "$out" | tail -8 >> /tmp/soak_bad.log; fi
  done
  s=$((s+1))
done
