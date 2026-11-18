# Virtual Steam Controller (remapper simulator)

The real `app/src/remap.c` and `sc26re_cfg.c` built for the PC, with the rest of the
firmware replaced by stubs that record what would go out (keyboard/mouse reports,
haptics, beeps, LED). Profiles go in through the same feature reports as on hardware.

    make                       # builds libsim.so
    python3 test_remap.py      # scenario tests (buttons, sticks, pads, triggers, layers, macros…)
    python3 test_remap2.py     # edge cases (stuck keys, flick stick, dead zones, swipe repeat…)
    python3 sim_server.py &    # virtual controller over HTTP (port 8765)
    node e2e_web.js            # web/remap.html in headless Chrome driving it (webhid_shim.js)
