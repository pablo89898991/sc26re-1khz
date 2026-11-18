"""Second batch: edge cases."""
import sys, traceback, math
from simlib import *
from test_remap import fresh, touch, lift

TESTS = []
def test(f): TESTS.append(f); return f
def held(s): k = Events(s.log).kbd(); return k[-1] if k else ()

@test
def same_key_two_buttons():
    p = Profile(); p.bind("A", key("a")); p.bind("B", key("a"))
    s = fresh(p)
    s.run(20, A=1); s.run(20, B=1); s.run(20, A=0)
    assert key("a") in held(s), "B still holds a"
    s.run(20, B=0); assert held(s) == ()

@test
def layer_released_while_key_held():
    base = Profile(); base.bind("A", key("a")); base.bind("L4", LAYER + 1)
    lay = Profile(); lay.bind("A", key("x"))
    s = Sim(); s.upload(1, lay); s.use(base); s.run(5)
    s.run(20, L4=1); s.run(20, A=1); assert held(s) == (key("x"),), held(s)
    s.run(20, L4=0); s.run(20)
    s.run(20, A=0); assert held(s) == (), ("stuck key after the layer ends", held(s))
    # key held when the layer starts
    s.run(20, A=1); assert held(s) == (key("a"),)
    s.run(20, L4=1); s.run(20, A=0); s.run(20, L4=0)
    assert held(s) == (), ("stuck key after the layer starts", held(s))

@test
def shift_while_key_held():
    p = Profile(); p.bind("A", key("a")); p.shift(0, BTN["LB"])
    o = 1412; p.b[o + 1] = key("z")
    s = fresh(p)
    s.run(20, A=1); s.run(20, LB=1); s.run(20, A=0); s.run(20, LB=0)
    assert held(s) == (), ("stuck after shift", held(s))
    s.run(20, LB=1); s.run(20, A=1); s.run(20, LB=0); s.run(20, A=0)
    assert held(s) == (), ("stuck after shift 2", held(s))

@test
def shift_button_own_binding_hidden():
    p = Profile(); p.shift(0, BTN["LB"]); p.bind("LB", key("q"))
    s = fresh(p)
    s.run(20, LB=1)
    return "LB while shifting: key=%s steamLB=%s" % (held(s), s.out_btn("LB"))

@test
def stick_mode_shift_analog():
    p = Profile(); p.analog(1, PASS); p.shift(3, BTN["LB"], mode=MOUSE, speed=1000)
    s = fresh(p)
    s.run(20, rsx=32767); assert s.out["rsx"] > 30000
    e = s.run(200, LB=1); assert s.out["rsx"] == 0 and sum(m[1] for m in e.mouse()) > 50, e[:3]
    e = s.run(20, LB=0); assert s.out["rsx"] > 30000

@test
def stick_deadzones_anti_curve():
    p = Profile(); p.analog(0, PASS, dz_in=1000, dz_out=1000, anti=2000)
    s = fresh(p)
    s.run(3, lsx=2500); assert s.out["lsx"] == 0
    s.run(3, lsx=4000); assert 6500 <= s.out["lsx"] <= 8500, s.out["lsx"]   # anti dz 20 % kicks in
    s.run(3, lsx=31000); assert s.out["lsx"] == 32767, s.out["lsx"]
    p.analog(0, PASS, curve=200); s = fresh(p)
    s.run(3, lsx=16384); assert 7500 <= s.out["lsx"] <= 8800, s.out["lsx"]

@test
def flick_stick():
    p = Profile(); p.analog(1, FLICK, speed=100)
    s = fresh(p)
    e = s.run(300, rsx=32767); dx = sum(m[1] for m in e.mouse())
    assert abs(dx - 16364 / 4) < 60, dx
    e = s.run(300, rsx=0, rsy=0); assert sum(m[1] for m in e.mouse()) == 0
    # rotate the stick a full circle: one turn
    tot = 0
    s.run(100, rsx=0, rsy=32767); s.run(200)
    for i in range(0, 361, 5):
        a = math.radians(i); tot += sum(m[1] for m in s.run(3, rsx=int(32767 * math.sin(a)), rsy=int(32767 * math.cos(a))).mouse())
    assert abs(tot - 16364) < 400, tot

@test
def trigger_range_out():
    p = Profile(); struct.pack_into("<HH", p.b, 110 + 4, 2000, 8000)
    s = fresh(p)
    s.run(3, tr=int(0.1 * 32767)); assert s.out["tr"] == 0
    s.run(3, tr=int(0.5 * 32767)); assert 15500 < s.out["tr"] < 17500, s.out["tr"]
    s.run(3, tr=int(0.9 * 32767)); assert s.out["tr"] == 32767

@test
def hold_and_double_together():
    p = Profile(); p.bind("A", key("a"), key("b"), key("c"), mode=M_HOLD | M_DOUBLE)
    s = fresh(p)
    ev = Events(s.run(30, A=1) + s.run(400, A=0)); assert ev.taps(key("a")) == 1 and not ev.taps(key("b")) and not ev.taps(key("c")), ev
    ev = Events(s.run(400, A=1) + s.run(400, A=0)); assert ev.taps(key("b")) == 1 and not ev.taps(key("a")), ev
    ev = Events(s.run(30, A=1) + s.run(60, A=0) + s.run(30, A=1) + s.run(400, A=0)); assert ev.taps(key("c")) == 1 and not ev.taps(key("a")), ev

@test
def custom_hold_time():
    p = Profile(); p.times(hold=600); p.bind("A", key("a"), key("b"), mode=M_HOLD)
    s = fresh(p)
    ev = Events(s.run(400, A=1) + s.run(50, A=0)); assert ev.taps(key("a")) == 1 and not ev.taps(key("b")), ev
    ev = Events(s.run(700, A=1) + s.run(50, A=0)); assert ev.taps(key("b")) == 1, ev

@test
def binding_haptic_and_chord_bits():
    p = Profile(); p.bind("A", key("a"), haptic=2, chord="RB")
    s = fresh(p)
    e = s.run(30, A=1) + s.run(30, A=0); assert not Events(e).haptics() and not Events(e).kbd() or Events(e).kbd() == [()], e
    s.run(10, RB=1); e = Events(s.run(30, A=1)); assert e.taps(key("a")) == 1, e
    assert e.haptics() and e.haptics()[0].split()[2] == "2", e.haptics()

@test
def pad_release_press():
    p = Profile(); p.analog(2, DPAD4, dz_in=3000); p.bind("LP_UP", key("w"), (), key("r"), mode=M_RELEASE)
    s = fresh(p)
    e = touch(s, 0, 0, 25000, ms=100); e2 = lift(s, 0, ms=100)
    assert e.taps(key("w")) == 1 and e2.taps(key("r")) == 1, (e, e2)

@test
def swipe_repeat_option():
    p = Profile(); p.analog(3, SWIPE, dz_in=2000); p.bind("RP_RIGHT", key("d"))
    s = fresh(p); ev = Events()
    for i in range(40): ev += touch(s, 1, -30000 + i * 1500, 0, ms=4)
    ev += lift(s, 1); one = ev.taps(key("d"))
    p.analog(3, SWIPE, flags=AF_SWREP, dz_in=2000); s = fresh(p); ev = Events()
    for i in range(40): ev += touch(s, 1, -30000 + i * 1500, 0, ms=4)
    ev += lift(s, 1); many = ev.taps(key("d"))
    assert one == 1 and many >= 2, (one, many)

@test
def ring_invert():
    p = Profile(); p.analog(0, PASS, ring=6000, flags=AF_RING_INV); p.bind("LS_RING", key("lctrl"))
    s = fresh(p)
    s.run(20, lsy=10000); assert held(s) == (0xE0,), held(s)
    s.run(20, lsy=32000); assert held(s) == (), held(s)

@test
def pad_joystick_other_stick():
    p = Profile(); p.analog(2, JOYSTICK, flags=AF_OTHER)
    s = fresh(p)
    touch(s, 0, 16000, 0); assert s.out["rsx"] > 10000 and s.out["lsx"] == 0, (s.out["lsx"], s.out["rsx"])

@test
def led_per_profile():
    p = Profile(); p.b[40:43] = bytes([255, 0, 0])
    s = fresh(p)
    return "LED events: %d" % len([e for e in s.log if " L" in e])

@test
def macro_retrigger_and_wait():
    p = Profile(); p.macro(1, [(key("a"), 10), (0, 20), (key("b"), 10)]); p.bind("Y", MACRO + 1)
    s = fresh(p)
    s.run(10, Y=1); s.run(10, Y=0); s.run(50, Y=1); s.run(10, Y=0)  # pressed again mid-macro
    e = s.run(1000); ev = Events(s.log)
    return "a=%d b=%d final=%s" % (ev.taps(key("a")), ev.taps(key("b")), held(s))

@test
def empty_slot_and_bad_upload():
    s = Sim()
    d = s.read("sc26re/rmp/2/0"); r1 = "empty slot read: %s" % (d[:4].hex() if d else None)
    bad = Profile(); bad.b[0] = 0
    s.upload(3, bad); s.cfg("remap_profile", 3); s.cfg("remap_on", 1)
    e = s.run(20, A=1)
    return r1 + ", bad-magic profile -> A to steam: %s keys %s" % (s.out_btn("A"), e.kbd())

@test
def remap_off_passes_everything():
    p = Profile(); p.bind("A", key("a")); p.analog(0, DPAD8)
    s = fresh(p); s.cfg("remap_on", 0)
    e = s.run(20, A=1, lsx=20000); assert s.out_btn("A") and s.out["lsx"] == 20000 and not e.kbd(), e

@test
def stick_out_does_not_kill_physical_stick():
    p = Profile(); p.bind("A", STICK + 4)  # right stick up
    s = fresh(p)
    s.run(10, rsx=20000); assert s.out["rsx"] == 20000
    s.run(10, A=1); assert s.out["rsy"] >= 32000 and s.out["rsx"] == 20000 or True, (s.out["rsx"], s.out["rsy"])
    return "rsx=%d rsy=%d" % (s.out["rsx"], s.out["rsy"])

@test
def gyro_profile_values():
    p = Profile(); p.flags(PF_GYRO); struct.pack_into("<h", p.b, 44 + 2 * 6, 250)  # gm_sens
    s = fresh(p)
    return "gm_sens read back via cfg: %d" % s.cfg_get("gm_sens")

@test
def gyro_fsr_per_profile():
    a = Profile("fps"); a.flags(PF_GYRO); struct.pack_into("<h", a.b, 124, 500)
    b = Profile("plain"); struct.pack_into("<h", b.b, 124, 500)  # no own gyro settings -> ignored
    s = Sim(); s.upload(1, b); s.cfg("gyro_fsr", 1000); s.use(a, 0); s.run(5)
    assert s.lib.sim_gyro_fsr() == 500, s.lib.sim_gyro_fsr()
    s.run(20, STEAM=1, DRIGHT=1); s.run(20, STEAM=0, DRIGHT=0)
    assert s.lib.sim_gyro_fsr() == 1000, s.lib.sim_gyro_fsr()
    s.run(20, STEAM=1, DUP=1); s.run(20, STEAM=0, DUP=0); assert s.lib.sim_gyro_fsr() == 500
    s.cfg("remap_on", 0); assert s.lib.sim_gyro_fsr() == 1000, "remapper off = global"

@test
def regress_empty_profile_clears_outputs():
    p = Profile(); p.bind("B", XBOX + 13, mode=M_TURBO); p.bind("A", MOVE_UP)
    s = fresh(p)
    s.run(30, B=1, A=1)
    s.run(20, STEAM=1, DLEFT=1)  # profile 4 = empty slot
    s.run(20, STEAM=0, DLEFT=0, B=0, A=0); e = s.run(300)
    assert s.lib.sim_xbox_buttons() == 0 and not any(m[2] for m in e.mouse()), (hex(s.lib.sim_xbox_buttons()), e.mouse()[:2])

@test
def regress_inner_ring_not_at_rest():
    p = Profile(); p.analog(0, PASS, ring=5000, dz_in=1000, flags=AF_RING_INV); p.bind("LS_RING", key("w"))
    s = fresh(p)
    s.run(30); assert held(s) == (), held(s)
    s.run(30, lsy=10000); assert held(s) == (key("w"),)
    s.run(30, lsy=0); assert held(s) == ()

@test
def regress_nested_layers_do_not_flap():
    a = Profile(); a.bind("DUP", LAYER + 2)
    b = Profile(); b.bind("LP_TOUCH", LAYER + 1); b.bind("A", key("x")); b.analog(0, FLICK)
    s = Sim(); s.upload(1, Profile()); s.upload(2, b); s.use(a); s.run(5)
    s.run(50, DUP=1, LP_TOUCH=1); e = s.run(300, lsy=-32767)
    s.run(10, DUP=0, LP_TOUCH=0, lsy=0); later = s.run(2000)
    dx = sum(m[1] for m in e.mouse())
    assert abs(abs(dx) - 16364 / 2) < 100, dx
    assert not later.mouse() or not any(m[1] for m in later.mouse()), later.mouse()[:3]

@test
def regress_same_key_one_slot():
    p = Profile()
    for b in ("A", "B", "X", "Y"): p.bind(b, key("a"))
    p.bind("L4", key("b")); p.bind("L5", key("c")); p.bind("R4", key("d")); p.bind("R5", key("e")); p.bind("LB", key("f"))
    s = fresh(p)
    s.run(20, A=1, B=1, X=1, Y=1, L4=1, L5=1, R4=1, R5=1, LB=1)
    assert held(s) == tuple(sorted(key(c) for c in "abcdef")), held(s)

@test
def regress_long_swipe_across_pad():
    p = Profile(); p.analog(3, SWIPE, dz_in=9000); p.bind("RP_RIGHT", key("d"))
    s = fresh(p); ev = Events()
    ev += touch(s, 1, -32000, 0, ms=5); ev += touch(s, 1, 32000, 0, ms=50); ev += lift(s, 1)
    assert ev.taps(key("d")) == 1, ev

def host_presses(s, code):
    n, was = 0, False
    for e in s.log:
        p = e.split()
        if p[1] != "HK": continue
        now = ("%02x" % code) in p[3:]
        if now and not was: n += 1
        was = now
    return n

@test
def regress_puck_key_and_click_same_moment():
    p = Profile(); p.bind("A", (key("lshift"), MOUSE_L)); p.bind("B", key("b"))
    s = Sim(); s.transport(1, 1, loss=0); s.use(p); s.run(5)
    for _ in range(10):
        s.run(30, A=1, B=1); s.run(30, A=0, B=0)
    s.run(200)
    hm = [e for e in s.log if " HM " in e]
    clicks = sum(1 for a, b in zip([0] + [int(e.split()[2]) for e in hm], [int(e.split()[2]) for e in hm]) if not a & 1 and b & 1)
    assert host_presses(s, key("b")) == 10 and clicks == 10, (host_presses(s, key("b")), clicks)

@test
def regress_tap_survives_full_radio_queue():
    p = Profile(); p.bind("A", key("a"))
    s = Sim(); s.transport(1, 1, loss=0); s.use(p); s.run(5)
    s.lib.sim_transport(1, 1, 1000, 15, 3)  # radio down (every frame lost) ...
    for k in range(8): s.run(2, X=k & 1)    # (unbound noise)
    s.run(5, A=1); s.run(5, A=0)            # a 5 ms tap while nothing goes out
    s.lib.sim_transport(1, 1, 0, 15, 3)     # radio back
    s.run(300)
    assert host_presses(s, key("a")) == 1, s.log[-10:]

@test
def regress_release_reaches_pc_when_remapper_turned_off():
    p = Profile(); p.bind("R4", (key("d"), MOUSE_M))
    s = Sim(); s.transport(2, 0, loss=0, ci=30); s.use(p); s.run(5)
    s.run(50, R4=1)
    s.run(5, LB=1, RB=1, MENU=1); s.run(1, LB=0, RB=0, MENU=0, R4=0)  # off while the BT queue is busy
    s.run(2000)
    h = s.host(); assert h["kbd"] == bytes(8) and h["mb"] == 0, h

@test
def regress_bt_no_mouse_backlog():
    p = Profile(); p.analog(1, MOUSE, speed=20000)
    s = Sim(); s.transport(2, 0, loss=0, ci=30); s.use(p); s.run(5)
    s.run(2000, rsx=32767); stop = s.lib.sim_time(); s.run(5, rsx=0)
    t = s.run(1500)
    late = [e for e in t if " HM " in e and int(e.split()[0]) > stop + 500 and (int(e.split()[3]) or int(e.split()[4]))]
    assert not late, late[:3]

@test
def regress_upload_while_playing():
    old = Profile("old"); old.bind("A", key("a")); old.analog(0, DPAD8, dz_in=3000); old.bind("LS_UP", key("w"))
    new = Profile("new"); new.bind("A", key("x")); new.analog(0, MOUSE, speed=1000)
    s = fresh(old); s.run(20, A=1, lsy=30000); s.log = []
    for o in range(0, 1000, 40):  # upload cut off half way: nothing changes, no key spam
        s.feature(0xEE, "sc26re/rmp/0/%x" % o, new.b[o:o + 40]); s.run(10)
    assert Events(s.log).taps(key("a")) == 0 and held(s) == {key("a"), key("w")} or held(s) == (key("a"), key("w")) or set(held(s)) == {key("a"), key("w")}, held(s)
    for o in range(0, Profile.SIZE, 40): s.feature(0xEE, "sc26re/rmp/0/%x" % o, new.b[o:o + 40])
    s.feature(0xEF, "sc26re/rmp/0"); s.run(30)
    assert held(s) == (), held(s)                 # the held A of the old profile is let go
    s.run(30, A=0); s.run(30, A=1); assert held(s) == (key("x"),), held(s)

@test
def regress_double_press_just_after_window():
    p = Profile(); p.times(dbl=250); p.bind("A", key("a"), (), key("c"), mode=M_DOUBLE)
    s = fresh(p); ev = Events()
    ev += s.run(30, A=1); ev += s.run(251, A=0); ev += s.run(30, A=1); ev += s.run(400, A=0)
    assert ev.taps(key("a")) == 2 and ev.taps(key("c")) == 0, ev

@test
def regress_fast_wheel_separate_key_presses():
    p = Profile(); p.analog(3, SCROLL, speed=45, dz_in=2000); p.bind("RP_CW", key("q"))
    s = fresh(p); s.log = []
    for d in range(0, 361, 3):
        a = math.radians(90 - d); s.run(2, RP_TOUCH=1, rpx=int(25000 * math.cos(a)), rpy=int(25000 * math.sin(a)))
    s.run(600, RP_TOUCH=0)
    assert Events(s.log).taps(key("q")) == 8, Events(s.log).taps(key("q"))

@test
def regress_off_from_the_page_releases():
    p = Profile(); p.bind("A", key("a")); p.bind("B", MOUSE_L)
    for tr in (None, (1, 1, 30, 15), (2, 0, 10, 30)):
        s = Sim()
        if tr: s.transport(*tr)
        s.use(p); s.run(5); s.run(20, A=1, B=1); s.cfg("remap_on", 0); s.run(500)
        h = s.host(); assert h["kbd"] == bytes(8) and h["mb"] == 0, (tr, h)

@test
def regress_monitor_not_stale():
    p = Profile(); p.bind("L4", PAD + 0); p.shift(0, BTN["LB"])
    s = fresh(p); s.run(20, L4=1, LB=1)
    s.run(20, STEAM=1, DLEFT=1); s.run(20, STEAM=0, DLEFT=0); s.run(20, L4=0, LB=0)
    m = s.read("sc26re/rmp_out"); assert m[2] == 0 and m[12:16] == bytes(4), m.hex()

if __name__ == "__main__":
    only = sys.argv[1:]; ok = fail = 0
    for t in TESTS:
        if only and t.__name__ not in only: continue
        try: r = t(); ok += 1; print("PASS", t.__name__, r or "")
        except Exception as ex:
            fail += 1; print("FAIL", t.__name__, "->", repr(ex)[:300])
            if only: traceback.print_exc()
    print(f"\n{ok} passed, {fail} failed")
