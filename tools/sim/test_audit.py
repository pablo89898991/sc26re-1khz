"""Whole-system audit (v51 test 44, 45): cases the fuzzers and reviews found, kept as fixed tests."""
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
    """the same on a trackpad: slice 10 of 10 chosen, lifted as a shift makes it 3 slices"""
    p = Profile(); p.analog(2, RADIAL, speed=10, dz_in=2000)
    p.shift(SH_LPAD, BTN["B"], mode=RADIAL, speed=3, dz_in=2000)
    # slice 10 of 10 and slice 1 of 3, as the shift has them (the shift is held when they fire)
    p.bind("LP_CW", key("q"), shifted_index=SHIFT_SRC.index(SRC["LP_CW"])); p.bind("LP_UP", key("1"), shifted_index=SHIFT_SRC.index(SRC["LP_UP"]))
    s = fresh(p)
    s.set(LP_TOUCH=1, lpx=-15000, lpy=26000); s.run(40)            # up, a little left: slice 10 of 10
    e = s.run(80, LP_TOUCH=0, lpx=0, lpy=0, B=1)                   # lifted as the shift makes it 3 slices
    got = set(c for k in e.kbd() for c in k)
    assert got == set(), ("the slice from the 10-slice menu is not in the 3-slice one", got)
    s.set(LP_TOUCH=1, lpx=0, lpy=25000); s.run(40); e = s.run(60, LP_TOUCH=0, lpx=0, lpy=0)
    assert set(c for k in e.kbd() for c in k) == {key("1")}, "the 3-slice menu works"

@test
def region_over_bluetooth_with_a_huge_screen_size():
    """a profile made elsewhere with a 65535 px screen: the way into the corner is capped (16384 px)"""
    p = Profile(); p.analog(1, REGION, dz_in=1000); p.screen(65535, 65535)
    s = Sim(); s.transport(2); s.link(1); s.use(p); s.run(50)
    s.run(100, rsx=20000)
    t0 = s.lib.sim_time(); last = t0
    for _ in range(200):
        e = s.run(50)
        if [x for x in e if x.split()[1] == "HM"]: last = s.lib.sim_time()
        elif s.lib.sim_time() - last > 500: break
    h = s.host()
    # with the cap the cursor goes 1.25 x 16384 up and comes back half of that: -12288; uncapped -49152
    assert -16384 <= h["dy"] <= 0, ("the way into the corner is not capped", h["dy"])
    assert last - t0 < 2500, ("the cursor still on its way after %d ms" % (last - t0))

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

# ---------------- tester reports ----------------
def turbo_run(p, tr, steps):
    s = Sim()
    if tr: s.transport(*tr)
    s.use(p); s.run(5); s.log = []
    for ms, a in steps: s.run(ms, A=a)
    hap = [e for e in s.log if e.split()[1] == "H"]
    kb = Events(s.log).kbd()
    return sum(1 for x, y in zip([()] + kb, kb) if key("a") in y and key("a") not in x), hap

@test
def turbo_repeats_play_the_binding_haptic():
    """v51 test 45, tester: turbo on a button with haptics ticked only on the first press. Every
    repeat is the binding firing again: one haptic per press, on USB, the Puck and Bluetooth"""
    for tr in (None, (1, 1, 0, 15), (2, 0, 0, 15)):
        p = Profile(); p.bind("A", key("a"), mode=M_TURBO, haptic=2); p.times(turbo=100)
        n, hap = turbo_run(p, tr, ((1000, 1), (300, 0), (1000, 1), (300, 0)))     # two holds
        assert n == 20 and len(hap) == n and all(h.split()[2] == "2" for h in hap), (tr, n, hap[:4])
    # the binding's own turbo period (50 ms), and turbo with toggle: a tick per press and one when switched off
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO, haptic=1); p.btime("A", turbo=50)
    n, hap = turbo_run(p, None, ((1000, 1), (300, 0)))
    assert n == 20 and len(hap) == 20, (n, len(hap))
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO | M_TOGGLE, haptic=3); p.times(turbo=100)
    n, hap = turbo_run(p, None, ((30, 1), (970, 0), (30, 1), (300, 0)))
    assert n == 10 and len(hap) == n + 1, (n, len(hap))
    # without haptics nothing ticks
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO); p.times(turbo=100)
    n, hap = turbo_run(p, None, ((1000, 1), (300, 0)))
    assert n == 10 and not hap, (n, hap[:3])

@test
def soft_pull_point_below_3_percent_lets_go():
    """v51 test 47, tester: a soft pull point under 3.5 % stayed on after the trigger was let go: it let
    go 3 % below the point, which is below 0. The hysteresis is half the point there, 3 % above 6 %"""
    T = lambda pct: int(32767 * pct / 100)
    for pct in (1, 2, 3, 3.5, 6, 50):
        p = Profile(); p.bind("LT_SOFT", key("q")); p.trig_soft(0, pct)
        s = fresh(p)
        s.run(50, tl=T(min(pct * 3, 100))); on = held(s)
        s.run(50, tl=0)
        assert on == (key("q"),) and held(s) == (), (pct, on, held(s))
    for pct, stays, goes in ((3, 2, 1), (50, 48, 46)):         # the hysteresis is still there
        p = Profile(); p.bind("LT_SOFT", key("q")); p.trig_soft(0, pct)
        s = fresh(p); s.run(50, tl=T(pct * 2))
        s.run(50, tl=T(stays)); a = held(s); s.run(50, tl=T(goes))
        assert a == (key("q"),) and held(s) == (), (pct, a, held(s))

@test
def gyro_tilt_below_3_degrees_lets_go():
    """the same for the gyro tilt directions: 3 degrees back from a tilt of 1 or 2 degrees stayed on
    with the controller level again (the page offers 3 and up, a hand-made profile anything)"""
    import math
    def tilt(fwd):
        a = math.radians(fwd); return dict(ax=0, ay=int(-16000 * math.sin(a)), az=int(16000 * math.cos(a)))
    for dz in (1, 2, 3, 15):
        p = Profile(); p.gyx(mode=1, tilt_dz=dz); p.bind("GYRO_UP", key("w"))
        s = fresh(p, **tilt(0)); s.run(30)
        s.run(30, **tilt(dz + 10)); on = held(s)
        s.run(30, **tilt(0))
        assert on == (key("w"),) and held(s) == (), (dz, on, held(s))
    p = Profile(); p.gyx(mode=1, tilt_dz=15); p.bind("GYRO_UP", key("w"))   # still 3 degrees of hysteresis
    s = fresh(p, **tilt(0)); s.run(30); s.run(30, **tilt(20))
    s.run(30, **tilt(13)); a = held(s); s.run(30, **tilt(11))
    assert a == (key("w"),) and held(s) == (), (a, held(s))

@test
def release_press_on_a_radial_slice():
    """v51 test 48, tester: "release press" on a radial menu slice never fired (a slice is a tap:
    pressed and let go at once). Its release set follows the press set, or comes alone"""
    def run(a0, src, pad):
        p = Profile()
        if pad: p.analog(2, RADIAL, dz_in=3000)
        else: p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=8)
        p.bind(src, a0, a2=key("q"), mode=M_RELEASE)
        s = Sim(); s.use(p); s.run(5); s.log = []
        if pad: s.run(40, LP_TOUCH=1, lpx=0, lpy=25000); s.run(200, LP_TOUCH=0, lpx=0, lpy=0)
        else: s.run(40, lsy=30000); s.run(200, lsy=0)
        return Events(s.log).kbd()
    for src, pad in (("LS_UP", False), ("LP_UP", True)):
        k = run((), src, pad)
        assert k == [(key("q"),), ()], (src, "release only", k)
        k = run(key("a"), src, pad)
        assert k == [(key("a"),), (), (key("q"),), ()], (src, "press, then release", k)

@test
def pad_click_belongs_to_a_mode_that_works_on_click():
    """v51 test 50, tester: left pad scroll wheel, shifted (L5) 9 zones that work while clicked; the
    click gave the zone's key and the pad's Click binding together. A mode that selects with the
    click owns it; with a mode on touch (or the scroll wheel) the Click binding acts as before"""
    def run(p, **hold):
        s = Sim(); s.use(p); s.run(5); s.log = []
        s.run(30, LP_TOUCH=1, lpx=0, lpy=0, **hold); s.run(50, LP_CLICK=1); s.run(50, LP_CLICK=0)
        s.run(30, LP_TOUCH=0, **{k: 0 for k in hold})
        return set(c for k in Events(s.log).kbd() for c in k)
    p = Profile(); p.analog(2, SCROLL); p.bind("LP_CLICK", key("q"))
    p.shift(4, BTN["L5"], mode=9, flags=AF_CLICK); p.bind(64, key("5"), shifted_index=29)   # shifted centre
    assert run(p, L5=1) == {key("5")}, ("shifted zones on click", run(p, L5=1))
    assert run(p) == {key("q")}, ("scroll wheel: the Click binding", run(p))
    p = Profile(); p.analog(2, 9, flags=AF_CLICK); p.bind("LP_CLICK", key("q")); p.bind("LP_CENTER", key("5"))
    assert run(p) == {key("5")}, run(p)
    p = Profile(); p.analog(2, 9); p.bind("LP_CLICK", key("q")); p.bind("LP_CENTER", key("5"))
    assert run(p) == {key("5"), key("q")}, ("zones on touch: the click is its own", run(p))

@test
def shifted_stick_and_pad_clicks_have_their_own_bindings():
    """v51 test 52 (profile v8), the user: a mode shift of a stick / trackpad also shifts its click (Steam
    replaces the whole pad, click included). Before, the click kept its normal binding in both modes"""
    def clicks(p, btn, hold):
        s = Sim(); s.use(p); s.run(5); s.log = []
        s.run(30, **hold); s.run(50, **{btn: 1}); s.run(50, **{btn: 0}); s.run(30, **{k: 0 for k in hold})
        return set(c for k in Events(s.log).kbd() for c in k)
    p = Profile(); p.analog(2, SCROLL); p.bind("LP_CLICK", key("q"))
    p.shift(4, BTN["L5"], mode=MOUSE); p.bind("LP_CLICK", key("w"), shifted_click=True)
    assert clicks(p, "LP_CLICK", {"L5": 1, "LP_TOUCH": 1}) == {key("w")}, "shifted pad click"
    assert clicks(p, "LP_CLICK", {"LP_TOUCH": 1}) == {key("q")}, "normal pad click"
    p = Profile(); p.bind("L3", key("a")); p.shift(2, BTN["L4"], mode=MOUSE); p.bind("L3", key("b"), shifted_click=True)
    assert clicks(p, "L3", {"L4": 1}) == {key("b")} and clicks(p, "L3", {}) == {key("a")}, "stick click"
    p = Profile(); p.bind("RP_CLICK", key("c")); p.shift(5, BTN["R5"], mode=MOUSE)                  # shifted click empty
    assert clicks(p, "RP_CLICK", {"R5": 1}) == set(), "an empty shifted click does nothing"
    # its own timing (v8 area): long press 500 ms on the shifted click, 250 ms (profile) on the normal one
    p = Profile(); p.bind("L3", key("a"), key("b"), mode=M_HOLD); p.shift(2, BTN["L4"], mode=MOUSE)
    p.bind("L3", key("c"), key("d"), mode=M_HOLD, shifted_click=True); p.b[Profile.SCLICK_BT] = 50
    def hold400(p, sh):
        s = Sim(); s.use(p); s.run(5); s.log = []
        s.run(20, **sh); s.run(400, L3=1); s.run(50, L3=0); s.run(20, **{k: 0 for k in sh})
        return set(c for k in Events(s.log).kbd() for c in k)
    assert hold400(p, {}) == {key("b")} and hold400(p, {"L4": 1}) == {key("c")}, (hold400(p, {}), hold400(p, {"L4": 1}))

@test
def v7_profile_keeps_its_click_in_a_mode_shift():
    """a v7 profile (older page / firmware): its click acted in both modes; uploaded to v8 firmware the
    shifted click is the normal one, so it still does"""
    p = Profile(); p.analog(2, SCROLL); p.bind("LP_CLICK", key("q")); p.shift(4, BTN["L5"], mode=MOUSE)
    b = bytearray(p.b[:3144]); b[1] = 7
    s = Sim()
    for o in range(0, len(b), 40):
        assert s.feature(0xEE, "sc26re/rmp/0/%x" % o, bytes(b[o:o + 40]))[0] == 0
    assert s.feature(0xEF, "sc26re/rmp/0")[0] == 0
    s.cfg("remap_profile", 0); s.cfg("remap_on", 1); s.run(5)
    assert s.download(0) == v8_of(bytes(b))
    s.log = []; s.run(30, L5=1, LP_TOUCH=1); s.run(50, LP_CLICK=1); s.run(50, LP_CLICK=0)
    assert key("q") in set(c for k in Events(s.log).kbd() for c in k)

# ---------------- found by mutation testing (mutate2.py old) ----------------
@test
def mouse_backlog_capped_after_a_trackpad_jump():
    """a trackpad as mouse at its fastest (400 %) and the finger 'jumping' across the pad in one
    report (a glitch): 256000 counts. At most 32767 wait (a quarter second over USB), the cursor
    must not run on for seconds"""
    p = Profile(); p.analog(3, MOUSE, speed=65535); p.anx(3, scale_x=400, scale_y=400)
    s = fresh(p)
    s.run(10, RP_TOUCH=1, rpx=-32000, rpy=-32000)
    s.run(1, rpx=32000, rpy=32000)
    s.run(10, RP_TOUCH=0, rpx=0, rpy=0); s.run(1000)
    h = s.host()
    assert 30000 < h["dx"] <= 32767 + 127 and -32767 - 127 <= h["dy"] < -30000, (h["dx"], h["dy"])

@test
def layer_keeps_the_extras_of_the_stick_that_holds_it():
    """the right stick's outer ring holds a layer: the stick keeps its own settings, also the v7
    extras (here the horizontal / vertical sensitivity), not the layer's 400 %"""
    base = Profile(); base.analog(1, MOUSE, speed=4000, ring=5000); base.bind("RS_RING", LAYER + 2); base.bind("A", key("a"))
    lay = Profile(); lay.analog(1, MOUSE, speed=4000); lay.anx(1, scale_x=400, scale_y=400); lay.bind("A", key("x"))
    s = Sim(); s.upload(2, lay); s.use(base); s.run(5)
    s.run(200, rsx=32767)
    e = Events(s.run(30, A=1)); s.run(20, A=0)
    assert key("x") in e.kbd()[-1], "the layer is on (held by the ring)"
    dx = sum(m[1] for m in s.run(500, rsx=32767).mouse())
    assert 1800 <= dx <= 2200, ("4000 counts/s at 100 %", dx)

# ---------------- the firmware's main loop (main.c is not in the simulator) ----------------
@test
def main_loop_hides_the_gyro_mouse_buttons_before_the_xbox_pad():
    """v51 test 45: the gyro mouse filter ran after the Xbox 360 output had taken the pad from the
    report: with the Xbox-only output the game saw LB + RB + View (the gyro chord), the ratchet and
    the quick turn buttons. It must also come before the "report changed" check."""
    import os
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../app/src/main.c")).read()
    f = src.index("current.buttons = gyro_mouse_filter_buttons(current.buttons, remap_raw_buttons());")
    assert src.count("gyro_mouse_filter_buttons(") == 1, "called once per report"
    assert src.index("remap_process(&current") < f < src.index("xinput_send(&current"), "filter after the Xbox output"
    assert f < src.index("bool changed = first"), "filter after the change check"

@test
def strong_haptic_is_stronger_than_click():
    """v51 test 46, tester: "strong" (remapper bindings, zones, gyro mouse) played the button pulse,
    which the Ibex renders with the tick waveform: weaker than "click". Now the click waveform with
    a gain above 0 dB (haptics.c and the Ibex backend are not in the simulator: the source is checked)"""
    import os, re
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../app/src/")
    h, ib = open(d + "haptics.c").read(), open(d + "haptics_pwm_ibex.c").read()
    fb = h[h.index("int haptics_play_feedback("):]; fb = fb[:fb.index("\n}\n")]
    assert re.search(r"case 3: return haptics_debug_play_strong\(", fb), "level 3 is not the strong click"
    st = h[h.index("int haptics_debug_play_strong("):]; st = st[:st.index("\n}\n")]
    g = re.search(r"#define HAPTICS_STRONG_CLICK_GAIN_DB (\d+)", h)
    assert "HAPTICS_EFFECT_STRONG_CLICK" in st and "HAPTICS_STRONG_CLICK_GAIN_DB" in st and g and int(g.group(1)) >= 3, st
    cs = ib[ib.index("case HAPTICS_EFFECT_STRONG_CLICK:"):]; cs = cs[:cs.index("case HAPTICS_EFFECT_TONE:")]
    assert "haptics_waveform_click" in cs and ".use_gain = true" in cs and "effect->gain_db" in cs, cs

@test
def xbox_class_descriptor_names_the_real_endpoints():
    """v51 test 53: the XInput class descriptor (0x21) names the IN / OUT endpoints. They were
    filled in only in xi_enable (after SET_CONFIGURATION), but Windows reads the configuration
    descriptor before that: it got 0x81 / 0x01, which belong to another interface, and the
    xusb22 pad never showed up in XInput. They must be filled in when the descriptor is built."""
    import os
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../app/src/xinput.c")).read()
    g = src[src.index("static void *xi_get_desc("):]; g = g[:g.index("\n}\n")]
    assert "xi_desc.xusb[6] = xi_desc.in_ep.bEndpointAddress" in g, g
    assert "xi_desc.xusb[13] = xi_desc.out_ep.bEndpointAddress" in g, g
    assert g.index("xusb[6]") < g.index("return xi_fs_desc"), g

@test
def turbo_repeats_the_long_press_set_too():
    """v51 test 55 (hardware test): turbo + long press held the long press set without repeating
    (turbo was only on the press set); Steam's turbo on a long press repeats it"""
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_HOLD | M_TURBO); p.times(hold=250, turbo=60)
    s = Sim(); s.use(p); s.run(5)
    e = Events(s.run(700, A=1) + s.run(200, A=0))
    assert e.taps(key("b")) >= 4 and e.taps(key("a")) == 0, (e.taps(key("a")), e.taps(key("b")))
    e = Events(s.run(80, A=1) + s.run(300, A=0))
    assert e.taps(key("a")) == 1 and e.taps(key("b")) == 0

@test
def stick_mouse_ticks_while_moving():
    """v51 test 55 (hardware test): the page offers "ticks while moving" for a stick in mouse mode,
    but only the trackpad mouse ticked"""
    p = Profile(); p.analog(0, MOUSE, speed=1000, haptic=1)
    s = Sim(); s.use(p); s.run(5)
    e = s.run(1000, lsx=32767)
    assert 5 <= len(e.haptics()) <= 12, len(e.haptics())
    e = s.run(500, lsx=0); assert not e.haptics()

if __name__ == "__main__":
    ok = bad = 0
    for t in TESTS:
        try: r = t(); ok += 1; print("PASS", t.__name__, r or "")
        except Exception as e: bad += 1; print("FAIL", t.__name__, e); traceback.print_exc()
    print("%d passed, %d failed" % (ok, bad))
    sys.exit(1 if bad else 0)
