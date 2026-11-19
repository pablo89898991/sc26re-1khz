"""Profile v7: per binding timing, stick radial / single / mouse region, pad region + edge spin,
gyro tilt / region, trigger curve, H/V sensitivity, v6 upgrade."""
import math, sys, traceback, math, struct
from simlib import *
from test_remap import touch, lift

TESTS = []
def test(f): TESTS.append(f); return f
def held(s): k = Events(s.log).kbd(); return k[-1] if k else ()
def times_of(s, code):
    """(start, end) ms of each press of a key, from the device log with time stamps"""
    return Events(s.log).kbd()

def key_timeline(s, code, ms, step=1, **kw):
    """run ms in steps, return the list of 0/1 'code held' per step"""
    out = []
    if kw: s.set(**kw)
    for _ in range(ms // step):
        s.run(step)
        out.append(1 if code in held(s) else 0)
    return out

def absr(e):
    """absolute pointer reports (x, y) of an event list"""
    return [tuple(int(v) for v in z.split()[2:4]) for z in e if z.split()[1] == "A"]

def fresh(prof, slot=0, **state):
    s = Sim()
    if state: s.set(**state)
    s.use(prof, slot)
    s.run(5)
    return s

def first(tl, v=1):
    return tl.index(v) if v in tl else None

# ---------------- per binding timing ----------------
@test
def per_binding_hold_time():
    p = Profile(); p.times(hold=250)
    p.bind("A", key("a"), key("b"), mode=M_HOLD); p.btime("A", hold=600)
    p.bind("B", key("c"), key("d"), mode=M_HOLD)
    s = fresh(p)
    tl = key_timeline(s, key("b"), 800, A=1)
    t = first(tl)
    assert t is not None and 580 <= t <= 620, ("A long press at", t)
    s.run(50, A=0); s.run(50)
    tl = key_timeline(s, key("d"), 400, B=1)
    t = first(tl)
    assert t is not None and 230 <= t <= 270, ("B (profile) long press at", t)
    s.run(50, B=0)

@test
def per_binding_double_window():
    p = Profile(); p.times(dbl=150)
    p.bind("A", key("a"), (), key("z"), mode=M_DOUBLE); p.btime("A", dbl=500)
    s = fresh(p)
    s.run(40, A=1); s.run(350, A=0); s.run(40, A=1)
    assert key("z") in held(s), ("second press after 350 ms within the 500 ms window", held(s))
    s.run(40, A=0); s.run(600)

@test
def per_binding_turbo():
    p = Profile(); p.times(turbo=100)
    p.bind("A", key("a"), mode=M_TURBO); p.btime("A", turbo=40)
    s = fresh(p)
    e = s.run(400, A=1)
    presses = sum(1 for k in e.kbd() if key("a") in k)
    s.run(20, A=0)
    assert 7 <= presses <= 12, ("40 ms turbo in 400 ms", presses)

@test
def delay_start_short_press():
    p = Profile(); p.bind("A", key("a")); p.btime("A", dstart=100)
    s = fresh(p)
    tl = key_timeline(s, key("a"), 30, A=1) + key_timeline(s, key("a"), 200, A=0)
    on = first(tl); off = tl.index(0, on) if on is not None else None
    assert on is not None and 95 <= on <= 106, ("delayed start", on)
    assert off is not None and 25 <= off - on <= 40, ("a 30 ms press stays 30 ms long", off - on)

@test
def delay_end():
    p = Profile(); p.bind("A", key("a")); p.btime("A", dend=200)
    s = fresh(p)
    s.run(50, A=1)
    tl = key_timeline(s, key("a"), 400, A=0)
    off = first(tl, 0)
    assert off is not None and 190 <= off <= 215, ("kept 200 ms after release", off)

@test
def interruptible_long_press():
    p = Profile()
    p.bind("A", key("a"), key("b"), mode=M_HOLD); p.btime("A", hold=300, interrupt=True)
    p.bind("X", key("x"))
    s = fresh(p)
    s.run(100, A=1); s.run(50, X=1); s.run(400)
    assert key("b") not in held(s), "long press interrupted by X"
    log0 = len(s.log)
    s.run(30, X=0); s.run(30, A=0); s.run(100)
    pressed = set(c for k in Events(s.log[log0:]).kbd() for c in k)
    assert key("a") not in pressed and key("b") not in pressed, ("nothing after an interruption", pressed)
    # not interrupted: works as before
    s.run(400, A=1)
    assert key("b") in held(s), held(s)
    s.run(50, A=0)

# ---------------- sticks ----------------
@test
def stick_radial_4():
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=4)
    p.bind("LS_UP", key("1")); p.bind("LS_RIGHT", key("2")); p.bind("LS_DOWN", key("3")); p.bind("LS_LEFT", key("4"))
    s = fresh(p)
    s.run(50, lsx=30000, lsy=0)
    assert held(s) == (), "nothing while pointing"
    e = s.run(80, lsx=0)
    got = set(c for k in e.kbd() for c in k)
    assert got == {key("2")}, ("right slice on return", got)
    assert s.out["lsx"] == 0, "Steam does not get the radial stick"
    e = s.run(50, lsx=0, lsy=-30000); e2 = s.run(80, lsy=0)
    assert set(c for k in e2.kbd() for c in k) == {key("3")}

@test
def stick_radial_8_diagonal():
    p = Profile(); p.analog(1, RADIAL, dz_in=3000); p.anx(1, slices=8)
    p.bind("RS_UR", key("u")); p.bind("RS_UP", key("1"))
    s = fresh(p)
    s.run(40, rsx=21000, rsy=21000); e = s.run(60, rsx=0, rsy=0)
    assert set(c for k in e.kbd() for c in k) == {key("u")}, e.kbd()

@test
def stick_radial_click_mode():
    p = Profile(); p.analog(0, RADIAL, flags=AF_CLICK, dz_in=3000); p.anx(0, slices=4)
    p.bind("LS_LEFT", key("4"))
    s = fresh(p)
    e = s.run(60, lsx=-30000); e = s.run(60, lsx=0)
    assert not e.kbd() or held(s) == (), "click mode: return does not fire"
    s.run(40, lsx=-30000); e = s.run(60, L3=1)
    assert any(key("4") in k for k in e.kbd()), e.kbd()
    s.run(40, L3=0, lsx=0)

@test
def stick_single_button():
    p = Profile(); p.analog(0, SINGLE, dz_in=4000); p.bind("LS_RING", key("w"))
    s = fresh(p)
    s.run(30, lsx=10000); assert held(s) == (), "inside the dead zone"
    s.run(30, lsx=0, lsy=20000); assert held(s) == (key("w"),), held(s)
    s.run(30, lsy=0); assert held(s) == ()

@test
def stick_region_usb():
    p = Profile(); p.analog(1, REGION, dz_in=1000); p.anx(1, region=(50, 50, 40, 40))
    s = fresh(p)
    e = s.run(20, rsx=32767, rsy=0)
    a = absr(e)
    assert a, "absolute reports"
    x, y = a[-1]
    assert abs(x - int(0.70 * 32767)) < 200 and abs(y - 16383) < 200, (x, y)
    e = s.run(20, rsx=0, rsy=32767)
    x, y = absr(e)[-1]
    assert abs(x - 16383) < 300 and abs(y - int(0.30 * 32767)) < 300, ("up = higher on the screen", x, y)
    e = s.run(20, rsy=0)
    x, y = absr(e)[-1]
    assert abs(x - 16383) < 50 and abs(y - 16383) < 50, ("back to the centre", x, y)
    e = s.run(50)
    assert not absr(e), "nothing while the stick rests"

@test
def stick_region_puck_relative():
    p = Profile(); p.analog(1, REGION, dz_in=1000); p.screen(1000, 500)
    s = Sim(); s.transport(1, 1); s.link(1); s.use(p); s.run(50)
    h0 = s.host()
    s.run(300, rsx=32767, rsy=0)
    h = s.host()
    dx, dy = h["dx"] - h0["dx"], h["dy"] - h0["dy"]
    # pushed into the corner (1.25 screens: -1250, -625) then to (1000, 250)
    assert abs(dx - (-1250 + 1000)) <= 3 and abs(dy - (-625 + 250)) <= 3, (dx, dy)
    s.run(300, rsx=0)
    h2 = s.host()
    assert abs((h2["dx"] - h["dx"]) - (-500)) <= 3, ("back to the centre", h2["dx"] - h["dx"])

@test
def stick_hv_scale():
    p = Profile(); p.analog(0, MOUSE, speed=1000); p.anx(0, scale_x=200, scale_y=50)
    s = fresh(p)
    e = s.run(200, lsx=32767); dx = sum(m[1] for m in e.mouse())
    e = s.run(200, lsx=0, lsy=-32767); dy = sum(m[2] for m in e.mouse())
    s.run(20, lsy=0)
    assert dx > 0 and dy > 0 and 3.0 < dx / dy < 5.0, ("x 200 % vs y 50 %", dx, dy)

# ---------------- trackpads ----------------
@test
def pad_region_and_return():
    p = Profile(); p.analog(2, REGION); p.anx(2, flags=1)   # REGION_RETURN
    s = fresh(p)
    e = touch(s, 0, -32767, 32767, ms=20)
    x, y = absr(e)[-1]
    assert x < 100 and y < 100, ("top-left corner", x, y)
    e = lift(s, 0)
    x, y = absr(e)[-1]
    assert abs(x - 16383) < 50 and abs(y - 16383) < 50, ("returned to the centre", x, y)

@test
def pad_region_stays():
    p = Profile(); p.analog(3, REGION)
    s = fresh(p)
    touch(s, 1, 16000, 0, ms=20)
    e = lift(s, 1)
    assert not absr(e), "stays where the finger left"

@test
def pad_edge_spin():
    p = Profile(); p.analog(3, MOUSE, speed=2000); p.anx(3, edge_spin=1000)
    s = fresh(p)
    touch(s, 1, 32000, 0, ms=10)
    e = s.run(200)
    dx = sum(m[1] for m in e.mouse())
    lift(s, 1)
    assert 120 <= dx <= 260, ("edge spin ~1000/s for 0.2 s", dx)
    p2 = Profile(); p2.analog(3, MOUSE, speed=2000)
    s2 = fresh(p2); touch(s2, 1, 32000, 0, ms=10); e = s2.run(200)
    assert sum(m[1] for m in e.mouse()) == 0, "no spin when off"

# ---------------- gyro ----------------
def gyro_prof(**kw):
    p = Profile(); p.gyx(**kw); return p

@test
def gyro_tilt_directions():
    p = gyro_prof(mode=1, tilt_dz=15)
    p.bind("GYRO_UP", key("w")); p.bind("GYRO_RIGHT", key("d")); p.bind("GYRO_LEFT", key("a"))
    s = fresh(p, ax=0, ay=-8000, az=14000)       # held tilted towards the player: that is the centre
    s.run(30)
    assert held(s) == (), held(s)
    def tilt(fwd, side):
        # rotate the reference gravity (0,-8000,14000) by fwd about X and side about Y
        a = math.radians(fwd); y, z = -8000, 14000
        y, z = y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a)
        b = math.radians(side); x = 0
        x, z = x * math.cos(b) - z * math.sin(b), x * math.sin(b) + z * math.cos(b)
        return dict(ax=int(x), ay=int(y), az=int(z))
    s.run(30, **tilt(25, 0)); assert held(s) == (key("w"),), ("forward = up", held(s))
    s.run(30, **tilt(0, 0)); assert held(s) == ()
    s.run(30, **tilt(0, 25)); r = held(s)
    s.run(30, **tilt(0, -25)); l = held(s)
    assert {r, l} == {(key("d"),), (key("a"),)}, ("left / right", r, l)
    s.run(30, **tilt(10, 0)); assert held(s) == (), "inside the dead zone"
    # 4-way: a diagonal tilt gives the bigger direction only; 8-way: both
    s.run(30, **tilt(30, 20)); assert held(s) == (key("w"),), ("4-way diagonal", held(s))
    s.run(30, **tilt(0, 0))
    s.run(30, **tilt(20, 30)); assert len(held(s)) == 1 and held(s) != (key("w"),), ("4-way diagonal, side bigger", held(s))
    s.run(30, **tilt(0, 0))
    p8 = gyro_prof(mode=1, tilt_dz=15, tilt_8way=1)
    p8.bind("GYRO_UP", key("w")); p8.bind("GYRO_RIGHT", key("d")); p8.bind("GYRO_LEFT", key("a"))
    s = fresh(p8, ax=0, ay=-8000, az=14000); s.run(30)
    s.run(30, **tilt(30, 25)); assert len(held(s)) == 2 and key("w") in held(s), ("8-way diagonal", held(s))

@test
def gyro_tilt_recentres_on_profile_change():
    p = gyro_prof(mode=1); p.bind("GYRO_UP", key("w"))
    s = Sim(); s.upload(1, p); s.use(p); s.set(ax=0, ay=0, az=16000); s.run(20)
    s.run(20, ay=-9000, az=13000)
    w1 = held(s)
    s.cfg("remap_profile", 1); s.run(30); s.cfg("remap_profile", 0); s.run(30)
    assert held(s) == (), ("the new position is the centre", w1, held(s))

@test
def gyro_region_usb():
    p = gyro_prof(mode=2, region_deg=60)
    s = fresh(p, gz=0)
    s.run(20)
    e = s.run(500, gz=int(30 / 0.06103))      # 30 deg/s for 0.5 s to the left = 15 deg = a quarter
    x, y = absr(e)[-1]
    assert abs(x - int(0.25 * 32767)) < 400 and abs(y - 16383) < 100, (x, y)
    e = s.run(3000, gz=int(60 / 0.06103))     # far past the edge: stops there
    x, y = absr(e)[-1]
    assert x == 0, x
    s.run(10, gz=0)

# ---------------- triggers ----------------
@test
def trigger_curve():
    p = Profile(); p.trig_curve(0, 200)
    s = fresh(p)
    s.run(10, tl=16384)
    assert 7800 <= s.out["tl"] <= 8600, ("50 % squared = 25 %", s.out["tl"])
    s.run(10, tl=32767); assert s.out["tl"] >= 32700
    s.run(10, tl=0)

# ---------------- layout ----------------
@test
def v6_profile_upgrades():
    p = Profile(); p.bind("A", key("a"))
    b = bytearray(p.b[:2056]); b[1] = 6
    s = Sim()
    for o in range(0, len(b), 40):
        assert s.feature(0xEE, "sc26re/rmp/0/%x" % o, bytes(b[o:o + 40]))[0] == 0
    assert s.feature(0xEF, "sc26re/rmp/0")[0] == 0
    s.cfg("remap_profile", 0); s.cfg("remap_on", 1); s.run(5)
    d = s.download(0)
    assert d == v8_of(bytes(b)), "v8 with an empty v7 area, the shifted clicks as the normal ones"
    s.run(20, A=1); assert held(s) == (key("a"),)
    s.run(20, A=0)

@test
def xbind_layer_and_shift_safe():
    # gyro / diagonal sources work in an action layer too
    base = Profile(); base.flags(PF_GYRO); base.gyx(mode=1); base.bind("L4", LAYER + 1)
    lay = Profile(); lay.bind("GYRO_UP", key("q"))
    s = Sim(); s.upload(1, lay); s.use(base); s.set(ax=0, ay=0, az=16000); s.run(20)
    s.run(20, L4=1); s.run(30, ay=-9000, az=13000)
    assert held(s) == (key("q"),), held(s)
    s.run(30, L4=0); s.run(30, ay=0, az=16000)
    assert held(s) == ()

@test
def stick_region_over_the_radio_ends_where_asked():
    """relative mouse region over the Puck / Bluetooth: corner first, then out - a stick flicked
    and let go must leave the cursor at the region centre, a held stick at its point"""
    W, H = 1920, 1080
    def run(link, loss, ci, script):
        p = Profile(); p.analog(0, REGION); p.anx(0, region=(50, 50, 50, 50))
        s = Sim(); s.transport(link, 1 if link == 1 else 0, loss=loss, ci=ci); s.use(p, 0); s.run(20); s.log.clear()
        x, y = 300.0, 800.0
        for kw, ms in script: s.run(ms, **kw)
        for e in s.log:
            f = e.split()
            if f[1] == "HM": x = min(max(x + int(f[3]), 0), W - 1); y = min(max(y + int(f[4]), 0), H - 1)
        return round(x), round(y)
    for link, loss, ci in [(1, 0, 15), (2, 0, 15), (1, 150, 15), (2, 100, 45)]:
        assert run(link, loss, ci, [({"lsx": 32767}, 30), ({"lsx": 0}, 5000)]) == (960, 540), link
        assert run(link, loss, ci, [({"lsx": 32767}, 5000)]) == (1440, 540), link
        assert run(link, loss, ci, [({"lsx": 32767}, 30), ({"lsx": 0}, 20), ({"lsy": 32767}, 40), ({"lsy": 0}, 5000)]) == (960, 540), link

@test
def start_press_with_long_press():
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_START | M_HOLD); p.btime(0, hold=300)
    s = Sim(); s.use(p); s.run(10); s.log.clear()
    s.run(100, A=1); ks = Events(s.log).kbd()
    assert any(key("a") in k for k in ks) and key("a") not in held(s), ("a tap at once", ks)
    s.run(250); assert held(s) == (key("b"),), held(s)
    s.run(40, A=0); assert held(s) == ()
    s.log.clear(); s.run(60, A=1); s.run(300, A=0)
    assert not any(key("b") in k for k in Events(s.log).kbd()), "short: no long press"

@test
def turbo_mouse_click_over_slow_links():
    """turbo on a mouse button: every click reaches the PC as its own click, also over a slow link"""
    for link, loss, ci in [(0, 0, 15), (1, 0, 15), (2, 0, 15), (1, 100, 15), (2, 50, 45)]:
        p = Profile(); p.bind("RB", MOUSE_L, mode=M_TURBO); p.times(turbo=40)
        s = Sim(); s.transport(link, 1 if link == 1 else 0, loss=loss, ci=ci); s.use(p); s.run(10); s.log.clear()
        s.run(1500, RB=1); s.run(2000, RB=0)
        dev = sum(1 for k, e in enumerate(s.log) if e.split()[1] == "M" and int(e.split()[2]) & 1 and not any(f.split()[1] == "M" and int(f.split()[2]) & 1 for f in s.log[max(0, k - 1):k]))
        presses = lambda kind: sum(1 for a, b2 in zip([0] + [int(e.split()[2]) & 1 for e in s.log if e.split()[1] == kind], [int(e.split()[2]) & 1 for e in s.log if e.split()[1] == kind]) if b2 and not a)
        d, h = presses("M"), presses("HM")
        assert d >= 5 and d == h and s.host()["mb"] == 0, (link, loss, d, h)

@test
def macro_with_all_sixteen_steps_ends():
    p = Profile(); p.macro(0, [(key("a") if k % 2 == 0 else 0, 2) for k in range(16)]); p.bind("A", MACRO + 0)
    s = Sim(); s.use(p); s.run(10); s.log.clear()
    s.run(30, A=1); s.run(30, A=0); s.run(800)
    assert Events(s.log).taps(key("a")) == 8 and held(s) == (), (Events(s.log).taps(key("a")), held(s))

@test
def wheel_left_and_right():
    p = Profile(); p.bind("X", 0x74); p.bind("B", 0x75)
    s = Sim(); s.use(p); s.run(10); h0 = s.host()["pan"]
    s.run(30, B=1); s.run(30, B=0); s.run(30); r = s.host()["pan"] - h0
    s.run(30, X=1); s.run(30, X=0); s.run(30); l = s.host()["pan"] - h0 - r
    assert r > 0 and l < 0, (r, l)

@test
def region_on_big_screens_over_the_radio():
    """2560 x 1440 and 3840 x 2160: the way into the corner and out is longer than the backlog a slow
    link may keep - the cursor must still end on its point, starting from the far corner"""
    for (W, H) in ((1920, 1080), (2560, 1440), (3840, 2160)):
        for link, loss, ci in ((1, 0, 15), (2, 0, 15), (2, 0, 45), (1, 150, 15), (2, 100, 45)):
            p = Profile(); p.analog(2, REGION); p.anx(2, region=(50, 50, 100, 100)); p.screen(W, H)
            s = Sim(); s.transport(link, 1 if link == 1 else 0, loss=loss, ci=ci); s.use(p, 0); s.run(20); s.log.clear()
            x, y = W - 1.0, H - 1.0
            want = []
            for (px, py) in ((30000, -30000), (-25000, 20000), (0, 0)):
                s.run(6000, LP_TOUCH=1, lpx=px, lpy=py)
                for e in s.log:
                    f = e.split()
                    if f[1] == "HM": x = min(max(x + int(f[3]), 0), W - 1); y = min(max(y + int(f[4]), 0), H - 1)
                s.log.clear()
                tx = (0.5 + px / 32767 * 0.5) * W; ty = (0.5 - py / 32767 * 0.5) * H
                assert abs(x - tx) <= 3 and abs(y - ty) <= 3, ((W, H), link, loss, ci, (px, py), (round(x), round(y)), (round(tx), round(ty)))
                s.run(100, LP_TOUCH=0, lpx=0, lpy=0)
                for e in s.log:
                    f = e.split()
                    if f[1] == "HM": x = min(max(x + int(f[3]), 0), W - 1); y = min(max(y + int(f[4]), 0), H - 1)
                s.log.clear()

@test
def flick_turns_arrive_whole_over_slow_links():
    """flick stick: a 180 degree flick and a full stick rotation are exact turns - on a slow link
    they may arrive later, never shorter (16364 counts per 360 in the simulator)"""
    for link, loss, ci in ((0, 0, 15), (1, 0, 15), (2, 0, 15), (2, 0, 45), (1, 150, 15), (2, 100, 45)):
        p = Profile(); p.analog(1, FLICK, dz_in=8000, speed=100)
        s = Sim(); s.transport(link, 1 if link == 1 else 0, loss=loss, ci=ci); s.use(p, 0); s.run(20)
        h0 = s.host()["dx"]
        s.run(60, rsx=0, rsy=-32767)          # flick backwards: 180 degrees
        s.run(30, rsx=0, rsy=0); s.run(6000)
        d = s.host()["dx"] - h0
        assert abs(abs(d) - 16364 / 2) <= 2, (link, loss, ci, "flick", d)
        h0 = s.host()["dx"]
        # point up, then turn the stick once round clockwise in 0.5 s, let go
        s.run(30, rsx=0, rsy=32767)
        for k in range(1, 101):
            a = math.radians(k * 3.6)
            s.run(5, rsx=int(32767 * math.sin(a)), rsy=int(32767 * math.cos(a)))
        s.run(30, rsx=0, rsy=0); s.run(6000)
        d = s.host()["dx"] - h0
        assert abs(d - 16364) <= 3, (link, loss, ci, "rotation", d)

if __name__ == "__main__":
    ok = fail = 0
    only = sys.argv[1:]
    for t in TESTS:
        if only and t.__name__ not in only: continue
        try:
            r = t(); ok += 1; print("PASS", t.__name__, r or "")
        except Exception as e:
            fail += 1; print("FAIL", t.__name__, e); traceback.print_exc(limit=2)
    print("\n%d passed, %d failed" % (ok, fail))
