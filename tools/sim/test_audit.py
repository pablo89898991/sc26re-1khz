"""Whole-system audit (v51 test 44): cases the fuzzers found, kept as fixed tests."""
import sys, traceback, struct
from simlib import *

TESTS = []
def test(f): TESTS.append(f); return f
def held(s): k = Events(s.log).kbd(); return k[-1] if k else ()
def fresh(prof, slot=0, **state):
    s = Sim()
    if state: s.set(**state)
    s.use(prof, slot); s.run(5)
    return s
SH_LS, SH_RS, SH_LPAD, SH_RPAD = 2, 3, 4, 5

@test
def stick_radial_slices_change_with_a_slice_chosen():
    """fuzz 9300 (UBSan): the left stick points at slice 7 of 8, then in the same report it comes
    back and a mode shift turns it into a 4-slice menu: the pending slice is not one of those 4"""
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=8)
    p.shift(SH_LS, BTN["B"], mode=RADIAL, dz_in=3000); p.anx(4 + SH_LS, slices=4)
    for src, k in (("LS_UP", "1"), ("LS_RIGHT", "2"), ("LS_DOWN", "3"), ("LS_LEFT", "4"), ("LS_UL", "5")):
        p.bind(src, key(k))
    s = fresh(p)
    s.run(40, lsx=-21000, lsy=21000)                 # up-left: slice 7 of 8
    e = s.run(80, lsx=0, lsy=0, B=1)                 # back to the centre as the shift turns it into 4 slices
    got = set(c for k in e.kbd() for c in k)
    assert got <= {key("5")}, got                    # the slice chosen in the 8-slice menu, or nothing
    s.run(20, B=0)
    s.run(40, lsx=30000, lsy=0); e = s.run(80, lsx=0)
    assert set(c for k in e.kbd() for c in k) == {key("2")}, "the menu still works"

@test
def pad_radial_slices_change_with_a_slice_chosen():
    """the same on a trackpad: 10 slices, then a shift with 3"""
    p = Profile(); p.analog(2, RADIAL, speed=10, dz_in=2000)
    p.shift(SH_LPAD, BTN["B"], mode=RADIAL, speed=3, dz_in=2000)
    s = fresh(p)
    s.set(LP_TOUCH=1, lpx=-20000, lpy=20000); s.run(40)   # slice 8 or 9 of 10
    s.set(LP_TOUCH=0, lpx=0, lpy=0, B=1); s.run(80)      # lifted as the shift makes it 3 slices
    s.run(20, B=0)
    s.set(LP_TOUCH=1, lpx=0, lpy=25000); s.run(40); s.set(LP_TOUCH=0, lpx=0, lpy=0); s.run(60)

# ---------------- live settings (sc26re_cfg.c) ----------------
AUTO = -32768
def cfg_raw(s, name):
    """value, min, max, can follow Steam"""
    r = s.read("sc26re/" + name); return struct.unpack("<hhhB", r[:7])

def stage(s, name, v):
    return s.feature(0xEE, "sc26re/" + name, struct.pack("<h", v))[0]

def registry(s, rid):
    v = ctypes.c_int16(0)
    return v.value if s.lib.sc26re_cfg_registry_override(rid, ctypes.byref(v)) else None

import ctypes
@test
def settings_clamp_round_and_default():
    s = Sim()
    for name, v, want in (("report_rate", 600, 500), ("report_rate", 800, 1000), ("report_rate", 300, 250), ("report_rate", 9999, 1000),
                          ("gm_sens", 0, 1), ("gm_sens", 30000, 5000), ("gyro_band_x_lo", 5, 0), ("gyro_band_x_hi", -5, 0),
                          ("trig_click", 10, 40), ("haptic_gain", -100, -24)):
        assert stage(s, name, v) == 0
        assert cfg_raw(s, name)[0] == want, (name, v, cfg_raw(s, name), want)
    # "default": the firmware default, or following Steam for the settings that can
    for name, follows in (("gm_sens", 0), ("trig_click", 1), ("report_rate", 0), ("auto_off", 1)):
        stage(s, name, 5); stage(s, name, AUTO); v, lo, hi, can = cfg_raw(s, name)
        assert can == follows and (v == AUTO) == bool(follows), (name, v, can)
    assert stage(s, "no_such_setting", 1) != 0

@test
def settings_steam_registry_overrides():
    s = Sim()
    TRIG, AUTO_OFF, HAPTIC, LED = 68, 50, 76, 45
    assert registry(s, TRIG) is None, "following Steam: no override"
    s.cfg("trig_click", 55); assert registry(s, TRIG) == 55
    s.cfg("auto_off", 10); assert registry(s, AUTO_OFF) == 600, "minutes -> seconds"
    s.cfg("auto_off", 0); assert registry(s, AUTO_OFF) == 32767, "never: the longest the registry has"
    s.cfg("haptic_gain", -12); assert registry(s, HAPTIC) == -12
    s.cfg("led_brightness", 0); assert registry(s, LED) == 0
    stage(s, "trig_click", AUTO); assert registry(s, TRIG) is None, "back to Steam's"

@test
def settings_survive_restart_and_bad_store():
    s = Sim()
    s.cfg("gm_sens", 250); s.cfg("trig_click", 61); s.cfg("remap_profile", 2)
    s.reboot(); s.run(5)
    assert cfg_raw(s, "gm_sens")[0] == 250 and cfg_raw(s, "trig_click")[0] == 61 and cfg_raw(s, "remap_profile")[0] == 2
    # values out of range or "follow Steam" for a setting that cannot, as an older firmware may have stored them
    for key, v in ((b"user/cfg/gm_sens", AUTO), (b"user/cfg/gyro_fsr", 9999), (b"user/cfg/remap_profile", 7), (b"user/cfg/trig_click", AUTO)):
        x = ctypes.c_int16(v); assert s.lib.settings_save_one(key, ctypes.byref(x), 2) == 0
    s.reboot(); s.run(5)
    assert cfg_raw(s, "gm_sens")[0] == 100, ("a setting that cannot follow Steam loads as its default", cfg_raw(s, "gm_sens"))
    assert cfg_raw(s, "gyro_fsr")[0] == 2000 and cfg_raw(s, "remap_profile")[0] == 3
    assert cfg_raw(s, "trig_click")[0] == AUTO, "this one may follow Steam"

if __name__ == "__main__":
    ok = bad = 0
    for t in TESTS:
        try: r = t(); ok += 1; print("PASS", t.__name__, r or "")
        except Exception as e: bad += 1; print("FAIL", t.__name__, e); traceback.print_exc()
    print("%d passed, %d failed" % (ok, bad))
    sys.exit(1 if bad else 0)
