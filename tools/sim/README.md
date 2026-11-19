# Virtual Steam Controller (remapper simulator)

The real `app/src/remap.c` and `sc26re_cfg.c` built for the PC, with the rest of the
firmware replaced by stubs that record what would go out (keyboard/mouse reports,
haptics, beeps, LED). Profiles go in through the same feature reports as on hardware.

    make                       # builds libsim.so
    python3 test_remap.py      # scenario tests (buttons, sticks, pads, triggers, layers, macros…)
    python3 test_remap2.py     # edge cases (stuck keys, flick stick, dead zones, swipe repeat…)
    python3 sim_server.py &    # virtual controller over HTTP (port 8765)
    node e2e_web.js            # web/remap.html in headless Chrome driving it (webhid_shim.js)
    bash battery.sh 9900       # everything: unit suites, properties, ASan fuzzing, USB / Puck / BT
                               # fuzzing, page e2e (meter, overlay, remapper); 9900 = seed
    bash soak.sh 9800          # endless fuzzing on new seeds, problems in /tmp/soak_bad.log
    python3 mutate2.py         # mutation testing: every mutant (one small bug) must fail a suite
    python3 mutate2.py old     # the same for the remap.c mutants of mutate.py
