#!/bin/bash
# full test battery; prints one line per suite
cd "$(dirname "$0")"
A=$(gcc -print-file-name=libasan.so)
U=${VDF_DIR:-/mnt/user-data/uploads}
run() { name=$1; shift; out=$("$@" 2>&1 | grep -v "agent-proxy\|connect_rejected\|googleapis\|Allowed domains\|For details" | tail -1); echo "$name: $out"; }
( run test_imu_raw sh -c 'gcc -O2 -Wall -Wextra -Werror -I../../app/src -o /tmp/test_imu_raw test_imu_raw.c -lm && /tmp/test_imu_raw'
  for t in test_remap test_remap2 test_xinput test_v7 test_vdf test_vdf_games test_vdf_modes test_user_profiles test_audit test_hwtest_end test_ring_first; do run $t timeout 1500 python3 $t.py; done
  run prop timeout 900 python3 prop.py $1 $(( $1 + 30 )); run prop2 timeout 900 python3 prop2.py $1 $(( $1 + 60 )) ) > /tmp/b_units.log 2>&1 &
( run test_gyro env SIMLIB=libsim_gm.so timeout 900 python3 test_gyro.py; run "asan test_v7" env LD_PRELOAD=$A ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so timeout 1500 python3 test_v7.py; run "asan test_vdf_games" env LD_PRELOAD=$A ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so timeout 1500 python3 test_vdf_games.py ) > /tmp/b_asan.log 2>&1 &
( for sd in $1 $(( $1 + 1 )); do run "fuzz asan $sd" env LD_PRELOAD=$A ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_asan.so timeout 900 python3 fuzz.py $sd 20; done ) > /tmp/b_fuzz.log 2>&1 &
( run "fuzz gm asan" env LD_PRELOAD=$A ASAN_OPTIONS=detect_leaks=0 SIMLIB=libsim_gm_asan.so timeout 900 python3 fuzz.py $(( $1 + 5 )) 15 ) > /tmp/b_fuzzgm.log 2>&1 &
( for m in 0 1 2; do run "wfuzz $m" timeout 900 python3 wfuzz.py $1 $m x 10; done; run "wfuzz gm" env SIMLIB=libsim_gm.so timeout 900 python3 wfuzz.py $(( $1 + 3 )) 2 x 6 ) > /tmp/b_wfuzz.log 2>&1 &
( run protofuzz timeout 600 python3 protofuzz.py $1 $(( $1 + 100 )); for f in $U/5d181df4*.vdf $U/f802*.vdf $U/0289a2b4*.vdf; do run "vdf_fuzz $(basename $f | cut -c1-8)" node vdf_fuzz.js $1 $f; done ) > /tmp/b_proto.log 2>&1 &
wait
pgrep -f sim_server.py >/dev/null || (python3 sim_server.py 8765 >/tmp/srv.log 2>&1 &)
pgrep -f "http.server 8766" >/dev/null || (cd ../../web && python3 -m http.server 8766 >/tmp/http.log 2>&1 &)
sleep 1
fresh() { pkill -f "sim_server.py 8765"; sleep 0.5; (python3 sim_server.py 8765 >/tmp/srv.log 2>&1 &); sleep 1; }
( fresh; run e2e_layout timeout 900 node e2e_layout.js; fresh; run e2e_ui timeout 900 node e2e_ui.js; fresh; run e2e_web timeout 900 node e2e_web.js; fresh; run e2e_vdf timeout 600 node e2e_vdf.js $U/0289a2b4-3737914488_controller_config.vdf; fresh
  echo "e2e_vdf_all: $(timeout 900 node e2e_vdf_all.js $U/f802a6dc-3724992272_controller_config.vdf $U/5d181df4-3728087656_controller_config.vdf $U/592293e0-3759492888_controller_config.vdf 2>&1 | grep -c '^FAIL') FAIL"
  echo "e2e_dirty: $(timeout 300 node e2e_dirty.js 2>&1 | grep -c '^FAIL') FAIL"; fresh; echo "e2e_shiftsub: $(timeout 300 node e2e_shiftsub.js 2>&1 | tail -1)"; fresh; echo "e2e_fwtest: $(timeout 900 node e2e_fwtest.js 2>&1 | grep "^FAIL\|checks" | tr "\n" " ")"; fresh; echo "e2e_hwtest: $(timeout 900 node e2e_hwtest.js 2>&1 | grep '^FAIL\|checks' | tr '\n' ' ')"; fresh; echo "hwt self: $(python3 hwt.py self | grep -vc '^ok') bad"; echo "e2e_meter: $(timeout 600 node e2e_meter.js 2>&1 | grep '^FAIL\|checks' | tr '\n' ' ')"; fresh; echo "e2e_levels: $(timeout 600 node e2e_levels.js 2>&1 | grep '^FAIL\|checks' | tr '\n' ' ')"; fresh; echo "e2e_overlay2: $(timeout 300 node e2e_overlay2.js 2>&1 | tail -1)"; timeout 1500 node e2e_overlay.js > /tmp/b_overlay.log 2>&1; echo "e2e_overlay: $(grep -c '^FAIL' /tmp/b_overlay.log) FAIL $(grep '^FAIL' /tmp/b_overlay.log | head -3 | cut -c1-140 | tr '\n' '|')" ) > /tmp/b_e2e.log 2>&1
cat /tmp/b_units.log /tmp/b_asan.log /tmp/b_fuzz.log /tmp/b_fuzzgm.log /tmp/b_wfuzz.log /tmp/b_proto.log /tmp/b_e2e.log
grep -h "runtime error\|ERROR: AddressSanitizer" /tmp/b_*.log | head -3
