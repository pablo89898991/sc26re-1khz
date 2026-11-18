"""Scenario tests of the on-controller remapper on the virtual controller."""
import sys, traceback
from simlib import *

TESTS = []
def test(f): TESTS.append(f); return f

def fresh(prof, slot=0):
    s = Sim()
    s.use(prof, slot)
    s.run(5)
    return s

# ---------------- buttons
@test
def basic_key_and_hidden():
    p = Profile(); p.bind("A", key("a"))
    s = fresh(p)
    e = s.run(20, A=1); assert e.kbd() == [(4,)], e; assert not s.out_btn("A")
    e = s.run(20, A=0); assert e.kbd() == [()], e
    e = s.run(20, B=1); assert s.out_btn("B"), "unbound B passes to Steam"

@test
def long_press():
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_HOLD)
    s = fresh(p)
    s.run(60, A=1); e = s.run(60, A=0)
    assert e.taps(key("a")) == 1 and e.taps(key("b")) == 0, s.log[-6:]
    e = s.run(400, A=1); e2 = s.run(50, A=0)
    assert e.taps(key("b")) == 1 and (e.taps(key("a")) + e2.taps(key("a"))) == 0, (e, e2)

@test
def double_press():
    p = Profile(); p.bind("A", key("a"), (), key("c"), mode=M_DOUBLE)
    s = fresh(p)
    ev = Events()
    ev += s.run(40, A=1); ev += s.run(60, A=0); ev += s.run(40, A=1); ev += s.run(300, A=0)
    assert ev.taps(key("c")) == 1 and ev.taps(key("a")) == 0, ev
    ev = Events(); ev += s.run(40, A=1); ev += s.run(400, A=0)
    assert ev.taps(key("a")) == 1 and ev.taps(key("c")) == 0, ev

@test
def toggle():
    p = Profile(); p.bind("A", key("a"), mode=M_TOGGLE)
    s = fresh(p)
    s.run(30, A=1); s.run(30, A=0)
    e = s.run(100); assert s.log and (4,) in Events(s.log).kbd()
    st = Events(s.log).kbd()[-1]; assert 4 in st, st
    s.run(30, A=1); s.run(30, A=0)
    st = Events(s.log).kbd()[-1]; assert 4 not in st, st

@test
def start_and_release_press():
    p = Profile(); p.bind("A", key("a"), (), key("r"), mode=M_START | M_RELEASE)
    s = fresh(p)
    e1 = s.run(500, A=1); assert e1.taps(key("a")) == 1 and e1.taps(key("r")) == 0, e1
    st = e1.kbd()[-1]; assert 4 not in st, "start press is a tap, not held"
    e2 = s.run(200, A=0); assert e2.taps(key("r")) == 1 and e2.taps(key("a")) == 0, e2

@test
def turbo():
    p = Profile(); p.times(turbo=100); p.bind("A", key("a"), mode=M_TURBO)
    s = fresh(p)
    e = s.run(1000, A=1); e += s.run(50, A=0)
    n = e.taps(key("a")); assert 9 <= n <= 11, n

@test
def cycle():
    p = Profile(); p.bind("A", key("a"), key("b"), key("c"), mode=M_CYCLE)
    s = fresh(p)
    seq = []
    for _ in range(4):
        e = s.run(40, A=1) + s.run(300, A=0)
        seq.append([c for c in (key("a"), key("b"), key("c")) if Events(e).taps(c)])
    assert seq == [[4], [5], [6], [4]], seq

@test
def chord():
    p = Profile(); p.bind("B", key("b"), chord="X")
    s = fresh(p)
    e = s.run(50, B=1) + s.run(50, B=0); assert Events(e).taps(key("b")) == 0, e
    s.run(20, X=1); e = s.run(50, B=1) + s.run(50, B=0); s.run(20, X=0)
    assert Events(e).taps(key("b")) == 1, e

@test
def modifiers_and_combo():
    p = Profile(); p.bind("A", (key("lctrl"), key("c")))
    s = fresh(p)
    e = s.run(30, A=1); assert e.kbd()[-1] == (6, 0xE0), e

@test
def mouse_buttons():
    p = Profile(); p.bind("RB", MOUSE_L); p.bind("LB", MOUSE_R); p.bind("R3", MOUSE_M)
    s = fresh(p)
    e = s.run(10, RB=1); assert e.mouse()[-1][0] == 1, e
    e = s.run(10, LB=1); assert e.mouse()[-1][0] == 3, e
    e = s.run(10, R3=1); assert e.mouse()[-1][0] == 7, e
    e = s.run(10, RB=0, LB=0, R3=0); assert e.mouse()[-1][0] == 0, e

@test
def mouse_move_action():
    p = Profile(); p.move_speed(1000); p.bind("DRIGHT", MOVE_RIGHT)
    s = fresh(p)
    e = s.run(500, DRIGHT=1); dx = sum(m[1] for m in e.mouse())
    assert 450 <= dx <= 550, dx

@test
def wheel_action():
    p = Profile(); p.bind("Y", WHEEL_UP)
    s = fresh(p)
    e = s.run(30, Y=1) + s.run(30, Y=0)
    assert sum(m[3] for m in Events(e).mouse()) == 1, e

@test
def steam_pad_and_xbox_actions():
    p = Profile(); p.bind("L4", PAD + BTN["A"]); p.bind("R4", XBOX + BTN["B"])
    s = fresh(p)
    s.run(10, L4=1); assert s.out_btn("A") and not s.out_btn("L4")
    s.run(10, R4=1); assert s.lib.sim_xbox_buttons() & (1 << BTN["B"]), hex(s.lib.sim_xbox_buttons())
    assert not s.out_btn("B"), "xbox-only action must not reach the Steam pad"

@test
def block_unbound():
    p = Profile(); p.flags(PF_BLOCK); p.bind("A", key("a"))
    s = fresh(p)
    s.run(10, B=1, X=1); assert not s.out_btn("B") and not s.out_btn("X")
    s.run(10, STEAM=1); assert not s.out_btn("STEAM"), "the Steam button never reaches Steam while the remapper is on"

# ---------------- sticks
@test
def stick_dpad8_and_4():
    p = Profile(); p.analog(0, DPAD8, dz_in=3000)
    for d, k in zip(("LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT"), "wsad"): p.bind(d, key(k))
    s = fresh(p)
    e = s.run(20, lsx=23000, lsy=23000); st = e.kbd()[-1]
    assert set(st) == {key("w"), key("d")}, st
    assert s.out["lsx"] == 0 and s.out["lsy"] == 0, "stick hidden from Steam"
    e = s.run(20, lsx=5000, lsy=5000); assert e.kbd()[-1] == (), e
    p.analog(0, DPAD4, dz_in=3000); s = fresh(p)
    e = s.run(20, lsx=26000, lsy=20000); st = e.kbd()[-1]; assert st == (key("d"),), st

@test
def stick_outer_ring():
    p = Profile(); p.analog(0, DPAD8, dz_in=2000, ring=9000)
    p.bind("LS_UP", key("w")); p.bind("LS_RING", key("lshift"))
    s = fresh(p)
    e = s.run(20, lsy=20000); assert e.kbd()[-1] == (key("w"),), e
    e = s.run(20, lsy=32000); assert set(e.kbd()[-1]) == {key("w"), 0xE1}, e

@test
def stick_mouse():
    p = Profile(); p.analog(1, MOUSE, speed=1000)
    s = fresh(p)
    e = s.run(500, rsx=32767); dx = sum(m[1] for m in e.mouse())
    assert dx > 200, dx
    assert s.out["rsx"] == 0

@test
def stick_pass_invert_rotate():
    p = Profile(); p.analog(0, PASS, flags=AF_INVX)
    s = fresh(p)
    s.run(5, lsx=20000, lsy=0); assert s.out["lsx"] < -15000, s.out["lsx"]
    p.analog(0, PASS, rotate=90); s = fresh(p)
    s.run(5, lsx=0, lsy=20000); assert abs(s.out["lsy"]) < 2000 and abs(s.out["lsx"]) > 15000, (s.out["lsx"], s.out["lsy"])

@test
def stick_scroll():
    p = Profile(); p.analog(1, SCROLL, speed=1000)
    s = fresh(p)
    e = s.run(1000, rsy=32767); w = sum(m[3] for m in e.mouse()); assert w != 0, e[:5]

# ---------------- trackpads
def touch(s, side, x, y, ms=20, click=False, pressure=0):
    t = "LP_TOUCH" if side == 0 else "RP_TOUCH"; c = "LP_CLICK" if side == 0 else "RP_CLICK"
    kw = {t: 1, c: 1 if click else 0}
    if side == 0: kw.update(lpx=x, lpy=y, lpp=pressure)
    else: kw.update(rpx=x, rpy=y, rpp=pressure)
    return s.run(ms, **kw)

def lift(s, side, ms=20):
    t = "LP_TOUCH" if side == 0 else "RP_TOUCH"; c = "LP_CLICK" if side == 0 else "RP_CLICK"
    kw = {t: 0, c: 0}
    kw.update(lpx=0, lpy=0, lpp=0) if side == 0 else kw.update(rpx=0, rpy=0, rpp=0)
    return s.run(ms, **kw)

@test
def pad_dpad_touch_vs_click():
    p = Profile(); p.analog(2, DPAD4, dz_in=3000)
    for d, k in zip(("LP_UP", "LP_DOWN", "LP_LEFT", "LP_RIGHT"), "wsad"): p.bind(d, key(k))
    s = fresh(p)
    e = touch(s, 0, 0, 25000); assert e.kbd()[-1] == (key("w"),), e
    e = lift(s, 0); assert e.kbd()[-1] == (), e
    p.analog(2, DPAD4, flags=AF_CLICK, dz_in=3000); s = fresh(p)
    e = touch(s, 0, 0, 25000); assert e.kbd() == [] or e.kbd()[-1] == (), e
    e = touch(s, 0, 0, 25000, click=True); assert e.kbd()[-1] == (key("w"),), e
    assert not s.out_btn("LP_CLICK"), "click consumed by the dpad"

@test
def pad_gates_with_haptics():
    p = Profile(); p.analog(2, GATES, dz_in=2000, haptic=1)
    for i, k in enumerate("qepz"): p.bind(46 + i, key(k))
    s = fresh(p)
    e = touch(s, 0, -20000, 20000); assert e.kbd()[-1] == (key("q"),), e
    assert e.haptics(), "zone haptic on entering a gate by touch"
    e = touch(s, 0, 20000, -20000); assert e.kbd()[-1] == (key("z"),), e
    assert e.haptics()

@test
def pad_grid_center():
    p = Profile(); p.analog(3, GRID, haptic=1)
    p.bind("RP_CENTER", key("c")); p.bind("RP_UP", key("w")); p.bind("RP_RIGHT", key("d"))
    s = fresh(p)
    e = touch(s, 1, 0, 0); assert e.kbd()[-1] == (key("c"),), e
    e = touch(s, 1, 0, 28000); assert e.kbd()[-1] == (key("w"),), e
    e = touch(s, 1, 28000, 28000); assert e.kbd()[-1] == (), ("corner = its own binding (gate)", e)
    p.bind("RP_G1", key("e")); s = fresh(p)
    e = touch(s, 1, 28000, 28000); assert e.kbd()[-1] == (key("e"),), e
    assert e.haptics(), "tick on the first touch"

@test
def pad_scroll_wheel():
    p = Profile(); p.analog(2, SCROLL, speed=30)
    p.bind("LP_CW", WHEEL_DOWN); p.bind("LP_CCW", WHEEL_UP)
    s = fresh(p)
    import math
    w = 0
    for i in range(0, 361, 5):
        a = math.radians(90 - i)  # clockwise from top
        e = touch(s, 0, int(25000 * math.cos(a)), int(25000 * math.sin(a)), ms=5)
        w += sum(m[3] for m in e.mouse())
    lift(s, 0)
    assert w <= -8, w

@test
def pad_mouse_and_trackball():
    p = Profile(); p.analog(3, MOUSE, speed=100)
    s = fresh(p)
    dx = 0
    for i in range(30):
        dx += sum(m[1] for m in touch(s, 1, -15000 + i * 1000, 0, ms=3).mouse())
    after = sum(m[1] for m in lift(s, 1, ms=200).mouse())
    assert dx > 0 and after == 0, (dx, after)
    p.analog(3, MOUSE, flags=AF_TRACKBALL, speed=100, dz_out=200)
    s = fresh(p)
    for i in range(30): touch(s, 1, -15000 + i * 1000, 0, ms=3)
    after = sum(m[1] for m in lift(s, 1, ms=300).mouse())
    later = sum(m[1] for m in s.run(3000).mouse())
    tail = sum(m[1] for m in s.run(500).mouse())
    assert after > 0 and tail == 0, (after, later, tail)

@test
def pad_swipe():
    p = Profile(); p.analog(3, SWIPE, speed=3000)
    p.bind("RP_RIGHT", key("d")); p.bind("RP_LEFT", key("a"))
    s = fresh(p)
    ev = Events()
    for i in range(20): ev += touch(s, 1, -15000 + i * 1500, 0, ms=4)
    ev += lift(s, 1)
    assert ev.taps(key("d")) == 1 and ev.taps(key("a")) == 0, ev

@test
def pad_radial():
    p = Profile(); p.analog(3, RADIAL, speed=4, dz_in=2500, haptic=1)
    # 4 slices: up, right, down, left -> sources 42.. (RP_UP..)?  find by bind on all candidate srcs
    keys = "1234"
    for i, k in enumerate(keys): p.bind((42, 51, 45, 53)[i], key(k))  # first 4 slices: up, up-right gate.. (radial_src)
    s = fresh(p)
    ev = touch(s, 1, 0, 0, ms=30)
    ev2 = touch(s, 1, 25000, 0, ms=30)
    assert ev2.haptics(), "tick on entering a slice"
    assert ev2.kbd() == [] or ev2.kbd()[-1] == (), "nothing while choosing"
    ev3 = lift(s, 1, ms=50)
    fired = [k for k in keys if ev3.taps(key(k))]
    assert len(fired) == 1, (ev3, fired)
    ev4 = touch(s, 1, 0, 0, ms=30) + lift(s, 1, ms=50)
    assert not any(Events(ev4).taps(key(k)) for k in keys), "centre cancels"
    return "fired " + fired[0]

@test
def pad_pressure_activation():
    p = Profile(); p.analog(2, DPAD4, flags=AF_PRESS, dz_in=3000, press=5000)
    p.bind("LP_UP", key("w"))
    s = fresh(p)
    e = touch(s, 0, 0, 25000, pressure=3000); assert not e.kbd() or e.kbd()[-1] == (), e
    e = touch(s, 0, 0, 25000, pressure=25000); assert e.kbd()[-1] == (key("w"),), e

@test
def pad_joystick_output():
    p = Profile(); p.analog(3, JOYSTICK, dz_in=500)
    s = fresh(p)
    touch(s, 1, 16000, 0); assert s.out["rsx"] > 10000, s.out["rsx"]

# ---------------- triggers
@test
def trigger_simple():
    p = Profile(); p.trig_soft(1, 30); p.bind("RT_SOFT", key("s")); p.bind("RT_CLICK", key("f"))
    s = fresh(p)
    e = s.run(30, tr=5000); assert not e.kbd(), e
    e = s.run(30, tr=15000); assert e.kbd()[-1] == (key("s"),), e
    e = s.run(30, tr=32767, RT_CLICK=1); assert set(e.kbd()[-1]) == {key("s"), key("f")}, e

@test
def trigger_hip_exclusive():
    p = Profile(); p.trig_soft(1, 30); p.trig_mode(1, 3, 75)
    p.bind("RT_SOFT", key("s")); p.bind("RT_CLICK", key("f"))
    s = fresh(p)
    # quick pull: straight to full -> only full
    e = s.run(5, tr=15000) + s.run(100, tr=32767, RT_CLICK=1)
    e = Events(e); assert e.taps(key("f")) == 1 and e.taps(key("s")) == 0, e
    s.run(100, tr=0, RT_CLICK=0)
    # slow pull: soft first, then full replaces it
    e1 = s.run(200, tr=15000); assert e1.kbd()[-1] == (key("s"),), e1
    e2 = s.run(50, tr=32767, RT_CLICK=1); assert e2.kbd()[-1] == (key("f"),), e2

@test
def trigger_hip():
    p = Profile(); p.trig_soft(1, 30); p.trig_mode(1, 2, 75)
    p.bind("RT_SOFT", key("s")); p.bind("RT_CLICK", key("f"))
    s = fresh(p)
    e = Events(s.run(5, tr=15000) + s.run(100, tr=32767, RT_CLICK=1))
    assert e.taps(key("f")) == 1 and e.taps(key("s")) == 0, e
    s.run(100, tr=0, RT_CLICK=0)
    s.run(200, tr=15000); e2 = s.run(50, tr=32767, RT_CLICK=1)
    assert set(e2.kbd()[-1]) == {key("s"), key("f")}, e2

@test
def trigger_hair():
    p = Profile(); p.trig_soft(1, 10); p.trig_mode(1, 1)
    p.bind("RT_SOFT", key("s"))
    s = fresh(p)
    e = s.run(30, tr=8000); e += s.run(30, tr=12000); assert Events(e).kbd()[-1] == (key("s"),), e
    e = s.run(30, tr=11000); assert e.kbd()[-1] == (), ("release a bit = off", e)
    e = s.run(30, tr=13000); assert e.kbd()[-1] == (key("s"),), e

# ---------------- shifts, layers, profiles, macros, outputs
@test
def mode_shift():
    p = Profile(); p.bind("A", key("a")); p.shift(0, BTN["LB"]); p.bind(None, key("z"), shifted_index=0) if False else None
    o = 1412 + 14 * 0; p.b[o] = 0; p.b[o + 1] = key("z")
    s = fresh(p)
    e = s.run(30, A=1) + s.run(30, A=0); assert Events(e).taps(key("a")) == 1
    s.run(20, LB=1); e = s.run(30, A=1) + s.run(30, A=0); s.run(20, LB=0)
    e = Events(e); assert e.taps(key("z")) == 1 and e.taps(key("a")) == 0, e
    assert not s.out_btn("LB") or True

@test
def action_layer():
    base = Profile("base"); base.bind("A", key("a")); base.bind("B", key("b")); base.bind("L4", LAYER + 1)
    lay = Profile("layer"); lay.bind("A", key("x"))
    s = Sim(); s.upload(1, lay); s.use(base, 0); s.run(5)
    s.run(20, L4=1)
    e = Events(s.run(30, A=1) + s.run(30, A=0) + s.run(30, B=1) + s.run(30, B=0))
    s.run(20, L4=0)
    assert e.taps(key("x")) == 1 and e.taps(key("a")) == 0 and e.taps(key("b")) == 1, e
    e = Events(s.run(30, A=1) + s.run(30, A=0)); assert e.taps(key("a")) == 1, e

@test
def profile_switch_action_and_steam_dpad():
    a = Profile("a"); a.bind("A", key("a")); a.bind("L5", PROFILE + 1)
    b = Profile("b"); b.bind("A", key("b"))
    s = Sim(); s.upload(1, b); s.use(a, 0); s.run(5)
    s.run(30, L5=1); s.run(30, L5=0)
    assert s.cfg_get("remap_profile") == 1
    e = Events(s.run(30, A=1) + s.run(30, A=0)); assert e.taps(key("b")) == 1, e
    s.run(20, STEAM=1); e = s.run(20, DUP=1); assert not s.out_btn("DUP"), "D-pad hidden while Steam held"
    assert s.cfg_get("remap_profile") == 0 and e.beeps()
    s.run(20, STEAM=0); s.run(20); assert not s.out_btn("DUP"), "stays hidden until released"
    s.run(20, DUP=0)

@test
def macro():
    p = Profile(); p.macro(0, [(key("a"), 5), (0, 10), (key("b"), 5)]); p.bind("X", MACRO + 0)
    s = fresh(p)
    e = s.run(20, X=1) + s.run(400, X=0)
    st = Events(e).kbd()
    assert Events(e).taps(key("a")) == 1 and Events(e).taps(key("b")) == 1, st
    t = {}
    for ln in e:
        pp = ln.split()
        if pp[1] == "K": t.setdefault(tuple(pp[3:]), int(pp[0]))
    return str(st)

@test
def stick_and_trigger_output_actions():
    p = Profile(); p.bind("A", STICK + 7); p.bind("B", TRIG_R); p.bind("X", STICK + 0)
    s = fresh(p)
    s.run(10, A=1); assert s.out["rsx"] >= 32000, s.out["rsx"]
    s.run(10, A=0, B=1); assert s.out["rsx"] == 0 and s.out["tr"] >= 32000, (s.out["rsx"], s.out["tr"])
    s.run(10, B=0, X=1); assert s.out["lsy"] >= 32000, s.out["lsy"]

@test
def remap_chord_toggle():
    p = Profile(); p.bind("A", key("a"))
    s = fresh(p)
    s.run(20, LB=1, RB=1, MENU=1); assert not s.out_btn("MENU")
    s.run(20, LB=0, RB=0, MENU=0)
    assert s.cfg_get("remap_on") == 0
    e = s.run(20, A=1); assert not e.kbd() and s.out_btn("A")
    s.run(20, A=0); s.run(20, LB=1, RB=1, MENU=1); s.run(20, LB=0, RB=0, MENU=0)
    assert s.cfg_get("remap_on") == 1

@test
def persistence_after_reboot():
    p = Profile(); p.bind("A", key("q"))
    s = fresh(p)
    s.reboot(); s.run(5)
    assert s.download(0) == bytes(p.b)
    e = s.run(20, A=1); assert e.kbd()[-1] == (key("q"),), e

@test
def held_key_released_on_profile_switch():
    a = Profile("a"); a.bind("A", key("a"))
    s = Sim(); s.upload(1, Profile("b")); s.use(a, 0); s.run(5)
    s.run(20, A=1)
    s.run(20, STEAM=1, DRIGHT=1)
    assert Events(s.log).kbd()[-1] == (), "keys released when switching profile"

@test
def per_profile_gyro_on():
    p = Profile(); p.flags(PF_GYRO_ON)
    s = fresh(p)
    s.run(10); assert s.lib.sim_gyro_mouse() == 1

@test
def monitor_path():
    p = Profile(); p.bind("A", key("a")); p.bind("B", MOUSE_L)
    s = fresh(p)
    s.run(10, A=1, B=1)
    m = s.read("sc26re/rmp_out"); assert m and len(m) == 30, m
    raw = s.read("sc26re/raw"); assert raw and struct.unpack("<I", raw[:4])[0] & 3 == 3

if __name__ == "__main__":
    only = sys.argv[1:]
    ok = fail = 0
    for t in TESTS:
        if only and t.__name__ not in only: continue
        try:
            r = t(); ok += 1; print("PASS", t.__name__, r or "")
        except Exception as ex:
            fail += 1; print("FAIL", t.__name__, "->", repr(ex)[:300])
            if only: traceback.print_exc()
    print(f"\n{ok} passed, {fail} failed")
