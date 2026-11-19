"""Remapper hardware test: the same scripts on the real controller and in the simulator.

usage:  hwt.py export            write web/hwtests.json (profiles + scripts + what the simulator expects)
        hwt.py check RESULTS     compare a results file from web/hwtest.html with the simulator
        hwt.py self              every case in the simulator through the firmware's own hwtest code

A case is a profile, a few controller settings and a script of input frames (at most 40 steps,
10 s). The firmware (hwtest.c, test builds) plays the script in its report loop and records each
change of the remapper's output; the simulator runs the same hwtest.c around the same remap.c.
"""
import json, math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simlib import *  # noqa

STEP = 30
MAX_STEPS = 40
ENTRY = 30
SLOT = 3   # the test profile goes to Profile 4 (the page backs it up and restores it)

# controller settings the remapper reads; the page reads them from the controller and the
# simulator uses the same values
def cfg_names():
    import re
    src = open(os.path.join(HERE, "../../app/src/sc26re_cfg.c")).read()
    return re.findall(r'\[SC26RE_CFG_\w+\]\s*=\s*\{\s*"(\w+)"', src)


def frame(st):
    b = st.get("buttons", 0)
    for k, v in st.items():
        if k in BTN and v: b |= 1 << BTN[k]
    g = lambda k: int(st.get(k, 0))
    return struct.pack("<Ihhhhhhhhhhhh", b, g("tl"), g("tr"), g("lsx"), g("lsy"), g("rsx"), g("rsy"),
                       g("lpx"), g("lpy"), g("lpp") & 0xFFFF, g("rpx"), g("rpy"), g("rpp") & 0xFFFF)


def script_bytes(steps):
    assert 0 < len(steps) <= MAX_STEPS, len(steps)
    assert sum(ms for ms, _ in steps) <= 10000
    return b"".join(struct.pack("<H", ms) + frame(st) for ms, st in steps)


def parse_log(raw):
    out = []
    for o in range(0, len(raw) - ENTRY + 1, ENTRY):
        e = raw[o:o + ENTRY]
        t, mb, sh = struct.unpack_from("<HBB", e)
        kbd = e[4:12]
        pad, xb, dx, dy, wh = struct.unpack_from("<IIiih", e, 12)
        keys = sorted([0xE0 + i for i in range(8) if kbd[0] >> i & 1] + [c for c in kbd[2:] if c])
        out.append(dict(t=t, mb=mb, sh=sh, keys=keys, pad=pad, xb=xb, dx=dx, dy=dy, wh=wh))
    return out


# ---------------- simulator
def sim_run(case, cfg=None):
    s = Sim()
    for k, v in (cfg or {}).items():
        if k in ("remap_on", "remap_profile"): continue
        try: s.cfg(k, v)
        except AssertionError: pass  # a setting the simulator does not take
    for slot, hx in case.get("extra", {}).items():
        q = Profile(); q.b[:] = bytes.fromhex(hx); s.upload(int(slot), q)
    s.use(_prof(case), slot=SLOT)
    s.run(50)
    sb = bytes.fromhex(case["script"])
    for o in range(0, len(sb), 40):
        assert s.feature(0xEE, "sc26re/hwt", b"\x01" + struct.pack("<H", o) + sb[o:o + 40])[0] == 0, o
    assert s.feature(0xEE, "sc26re/hwt", bytes([2, len(sb) // STEP]))[0] == 0
    total = sum(struct.unpack_from("<H", sb, o)[0] for o in range(0, len(sb), STEP))
    s.run(total + 20)
    st = s.read("sc26re/hwt_st")
    n = struct.unpack_from("<H", st, 2)[0]
    s.feature(0xEE, "sc26re/hwt", b"\x04\x00\x00")
    raw = b""
    for i in range(n):
        d = s.read("sc26re/hwt_log"); assert struct.unpack_from("<H", d)[0] == i; raw += d[2:]
    s.feature(0xEE, "sc26re/hwt", b"\x03")
    return dict(flags=st[0], log=parse_log(raw))


def _prof(case):
    p = Profile(); p.b[:] = bytes.fromhex(case["profile"]); return p


# ---------------- comparison
def same(a, b, tol_ms=6):
    """a: hardware log, b: simulator log. Same digital outputs in the same order, close in time;
    mouse / wheel totals close at the end."""
    dig = lambda l: [(e["mb"], e["sh"], tuple(e["keys"]), e["pad"], e["xb"]) for e in l]
    da, db = dig(a), dig(b)
    # movement-only entries repeat the digital state: keep only changes
    ch = lambda l, d: [(e["t"], x) for e, x, prev in zip(l, d, [None] + d[:-1]) if x != prev]
    ca, cb = ch(a, da), ch(b, db)
    probs = []
    if [x for _, x in ca] != [x for _, x in cb]:
        probs.append("outputs differ:\n      controller: %s\n      simulator:  %s" % (fmt(ca), fmt(cb)))
    else:
        for (ta, x), (tb, _) in zip(ca, cb):
            if abs(ta - tb) > max(tol_ms, 0.05 * tb):
                probs.append("timing: %s at %d ms on the controller, %d ms in the simulator" % (fmtx(x), ta, tb))
    if a and b:
        for k in ("dx", "dy", "wh"):
            va, vb = a[-1][k], b[-1][k]
            if abs(va - vb) > max(3, 0.12 * abs(vb)):
                probs.append("mouse %s total: controller %d, simulator %d" % (k, va, vb))
    return probs


def fmtx(x):
    mb, sh, keys, pad, xb = x
    s = []
    if keys: s.append("keys " + "+".join("%02x" % k for k in keys))
    if mb: s.append("mouse %x" % mb)
    if sh: s.append("shift %x" % sh)
    if pad: s.append("pad %x" % pad)
    if xb: s.append("xbox %x" % xb)
    return ", ".join(s) or "nothing"


def fmt(c):
    return " | ".join("%d:%s" % (t, fmtx(x)) for t, x in c[:12]) + (" ..." if len(c) > 12 else "")


# ---------------- the cases
CASES = []
def case(name, what):
    def deco(f):
        CASES.append((name, what, f)); return f
    return deco


def S(ms, **st): return (ms, st)
N = lambda ms=150: S(ms)  # neutral


def ring_points(side, r, deg0, deg1, n, ms, click=False):
    pts = []
    for i in range(n + 1):
        a = math.radians(deg0 + (deg1 - deg0) * i / n)
        x, y = int(r * math.cos(a)), int(r * math.sin(a))
        st = {("lpx" if side == 0 else "rpx"): x, ("lpy" if side == 0 else "rpy"): y,
              ("LP_TOUCH" if side == 0 else "RP_TOUCH"): 1}
        if click: st["LP_CLICK" if side == 0 else "RP_CLICK"] = 1
        pts.append((ms, st))
    return pts


@case("buttons_basic", "A key, B left mouse, X wheel up, Y pad A, L4 Xbox B, multi-action R4 = shift+e")
def _():
    p = Profile(); p.bind("A", key("a")); p.bind("B", MOUSE_L); p.bind("X", WHEEL_UP); p.bind("Y", PAD + BTN["A"])
    p.bind("L4", XBOX + BTN["B"]); p.bind("R4", (key("lshift"), key("e")))
    return p, [N(50), S(120, A=1), N(), S(120, B=1), N(), S(120, X=1), N(), S(120, Y=1), N(), S(120, L4=1), N(),
               S(120, R4=1), N(), S(120, A=1, B=1, R4=1), N(200)]


@case("hold_double_tap", "A: tap a, hold b, double c")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), key("c"), mode=M_HOLD | M_DOUBLE); p.times(hold=300, dbl=250)
    return p, [N(50), S(60, A=1), N(400), S(500, A=1), N(400), S(60, A=1), N(80), S(60, A=1), N(500)]


@case("toggle_start_release", "A toggle, B start press, X release press")
def _():
    p = Profile(); p.bind("A", key("a"), mode=M_TOGGLE); p.bind("B", key("b"), mode=M_START); p.bind("X", key("x"), mode=M_RELEASE)
    return p, [N(50), S(100, A=1), N(200), S(100, A=1), N(200), S(300, B=1), N(200), S(300, X=1), N(300)]


@case("turbo", "A turbo key, B turbo Xbox A, timing 50 ms")
def _():
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO); p.bind("B", XBOX + BTN["A"], mode=M_TURBO); p.times(turbo=50)
    return p, [N(50), S(400, A=1), N(200), S(400, B=1), N(300)]


@case("cycle", "A cycles a, b, c")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), key("c"), mode=M_CYCLE)
    return p, [N(50)] + [x for _ in range(4) for x in (S(80, A=1), N(120))] + [N(200)]


@case("chord", "B with RB held: b; RB alone pad RB")
def _():
    p = Profile(); p.bind("B", key("b"), chord="RB")
    return p, [N(50), S(100, B=1), N(150), S(100, RB=1), S(150, RB=1, B=1), S(100, RB=1), N(200)]


@case("macro", "X runs a 50 ms, wait 100 ms, b 50 ms")
def _():
    p = Profile(); p.macro(0, [(key("a"), 5), (0, 10), (key("b"), 5)]); p.bind("X", MACRO + 0)
    return p, [N(50), S(40, X=1), N(500), S(40, X=1), N(60), S(40, X=1), N(500)]


@case("layer_hold", "L4 holds a layer (test profile in slot 3 with A = x, LS mouse)")
def _():
    p = Profile(); p.bind("A", key("a")); p.bind("L4", LAYER + 2)
    lay = Profile("layer"); lay.bind("A", key("x")); lay.analog(0, MOUSE, speed=1000)
    _extra[2] = lay
    return p, [N(50), S(100, A=1), N(100), S(100, L4=1), S(150, L4=1, A=1), S(100, L4=1), N(100), S(100, A=1), N(200)]


@case("triggers_soft_full", "LT soft 30 % key s, full click key f; RT full to Xbox RT")
def _():
    p = Profile(); p.trig_soft(0, 30); p.bind("LT_SOFT", key("s")); p.bind("LT_CLICK", key("f"))
    p.bind("RT_CLICK", MOUSE_R)
    st = [N(50)]
    for v in (3000, 8000, 12000, 20000, 32767):
        st.append(S(80, tl=v, LT_CLICK=1 if v == 32767 else 0))
    st += [S(80, tl=12000), S(80, tl=8000), S(80, tl=1000), N(100), S(120, tr=32767, RT_CLICK=1), N(200)]
    return p, st


@case("trigger_soft_hysteresis", "LT soft 3 % (the stuck soft pull bug)")
def _():
    p = Profile(); p.trig_soft(0, 3); p.bind("LT_SOFT", key("s"))
    return p, [N(50), S(100, tl=2000), S(100, tl=800), S(100, tl=300), N(150), S(100, tl=1500), S(100, tl=700), N(200)]


@case("stick_dpad4", "LS D-pad 4-way wasd, rolling round")
def _():
    p = Profile(); p.analog(0, DPAD4, dz_in=3000)
    for d, k in zip(("LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT"), "wsad"): p.bind(d, key(k))
    st = [N(50)]
    for deg in range(90, 90 + 361, 30):
        a = math.radians(deg); st.append(S(60, lsx=int(28000 * math.cos(a)), lsy=int(28000 * math.sin(a))))
    return p, st + [N(200)]


@case("stick_dpad8", "LS D-pad 8-way wasd, rolling round")
def _():
    p = Profile(); p.analog(0, DPAD8, dz_in=3000)
    for d, k in zip(("LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT"), "wsad"): p.bind(d, key(k))
    st = [N(50)]
    for deg in range(90, 90 + 361, 22):
        a = math.radians(deg); st.append(S(50, lsx=int(28000 * math.cos(a)), lsy=int(28000 * math.sin(a))))
    return p, st + [N(200)]


def _ring_case(mode, inv):
    p = Profile(); p.analog(0, mode, dz_in=3000, ring=8000, flags=AF_RING_INV if inv else 0)
    for d, k in zip(("LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT"), "wsad"): p.bind(d, key(k))
    p.bind("LS_RING", key("lshift"))
    st = [N(50)]
    for r in (20000, 31000):
        for deg in range(90, 90 + 361, 45):
            a = math.radians(deg); st.append(S(60, lsx=int(r * math.cos(a)), lsy=int(r * math.sin(a))))
        st.append(N(80))
    return p, st + [N(150)]

@case("stick_dpad8_outer_ring", "LS 8-way + outer ring shift (the direction change bug), inner then outer")
def _(): return _ring_case(DPAD8, False)

@case("stick_dpad4_inner_ring", "LS 4-way + ring inverted (inner)")
def _(): return _ring_case(DPAD4, True)


@case("stick_mouse", "RS mouse, speed 1000")
def _():
    p = Profile(); p.analog(1, MOUSE, speed=1000)
    return p, [N(50), S(500, rsx=32767), N(100), S(400, rsy=-20000, rsx=-10000), N(200)]


@case("stick_scroll", "RS scroll")
def _():
    p = Profile(); p.analog(1, SCROLL, speed=1000)
    return p, [N(50), S(800, rsy=32767), N(100), S(800, rsy=-32767), N(200)]


@case("stick_diagonals_ring_src", "LS 8-way diagonals bound to their own keys (LS_UR..)")
def _():
    p = Profile(); p.analog(0, DPAD8, dz_in=3000)
    for d, k in zip(("LS_UP", "LS_UR", "LS_RIGHT", "LS_DR", "LS_DOWN", "LS_DL", "LS_LEFT", "LS_UL"), "12345qwe"):
        p.bind(d, key(k))
    st = [N(50)]
    for deg in range(90, 90 - 361, -45):
        a = math.radians(deg); st.append(S(80, lsx=int(28000 * math.cos(a)), lsy=int(28000 * math.sin(a))))
    return p, st + [N(200)]


@case("stick_radial", "LS radial menu 8 slices, release fires")
def _():
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=8)
    for i, k in enumerate("12345qwe"): p.bind((30, 70, 33, 71, 31, 72, 32, 73)[i], key(k))
    return p, [N(50), S(150, lsx=28000), N(200), S(150, lsy=28000), S(150, lsx=-28000), N(200)]


@case("pad_dpad_touch_click", "LP D-pad by touch, RP D-pad needing a click (click owned)")
def _():
    p = Profile(); p.analog(2, DPAD4, dz_in=3000); p.analog(3, DPAD4, dz_in=3000, flags=AF_CLICK)
    for d, k in zip(("LP_UP", "LP_DOWN", "LP_LEFT", "LP_RIGHT"), "wsad"): p.bind(d, key(k))
    for d, k in zip(("RP_UP", "RP_DOWN", "RP_LEFT", "RP_RIGHT"), "ikjl"): p.bind(d, key(k))
    p.bind("RP_CLICK", key("space"))
    return p, [N(50), S(120, LP_TOUCH=1, lpy=25000), S(120, LP_TOUCH=1, lpx=25000), N(),
               S(120, RP_TOUCH=1, rpy=25000), S(120, RP_TOUCH=1, rpy=25000, RP_CLICK=1), N(),
               S(120, RP_TOUCH=1, rpx=-25000, RP_CLICK=1), N(200)]


@case("pad_grid", "RP 9-zone grid with click (the grid click bug)")
def _():
    p = Profile(); p.analog(3, GRID, flags=AF_CLICK)
    p.bind("RP_CENTER", key("c")); p.bind("RP_UP", key("w")); p.bind("RP_G1", key("e")); p.bind("RP_CLICK", key("space"))
    return p, [N(50), S(120, RP_TOUCH=1), S(120, RP_TOUCH=1, RP_CLICK=1), N(),
               S(120, RP_TOUCH=1, rpy=28000, RP_CLICK=1), N(), S(120, RP_TOUCH=1, rpx=28000, rpy=28000, RP_CLICK=1), N(200)]


@case("pad_radial", "RP radial, choose by sliding, fires on lift")
def _():
    p = Profile(); p.analog(3, RADIAL, speed=4, dz_in=2500)
    for i, k in enumerate("1234"): p.bind((42, 51, 45, 53)[i], key(k))
    return p, [N(50), S(60, RP_TOUCH=1), S(120, RP_TOUCH=1, rpx=25000), N(200), S(60, RP_TOUCH=1), N(200)]


@case("pad_scroll_wheel", "LP circle scroll: clockwise q, counter-clockwise e")
def _():
    p = Profile(); p.analog(2, SCROLL, speed=30); p.bind("LP_CW", key("q")); p.bind("LP_CCW", key("e"))
    return p, [N(50)] + ring_points(0, 25000, 90, -270, 16, 25) + [N(150)] + ring_points(0, 25000, 90, 450, 16, 25) + [N(200)]


@case("pad_mouse", "RP mouse, then trackball")
def _():
    p = Profile(); p.analog(3, MOUSE, speed=100)
    return p, [N(50)] + [S(6, RP_TOUCH=1, rpx=-15000 + i * 1000) for i in range(30)] + [N(300)]


@case("pad_swipe", "RP swipe right d, left a")
def _():
    p = Profile(); p.analog(3, SWIPE, speed=3000); p.bind("RP_RIGHT", key("d")); p.bind("RP_LEFT", key("a"))
    return p, ([N(50)] + [S(8, RP_TOUCH=1, rpx=-15000 + i * 1500) for i in range(18)] + [N(200)]
               + [S(8, RP_TOUCH=1, rpx=15000 - i * 1500) for i in range(18)] + [N(200)])


@case("pad_gates", "LP 4 gates")
def _():
    p = Profile(); p.analog(2, GATES, dz_in=2000)
    for i, k in enumerate("qepz"): p.bind(46 + i, key(k))
    return p, [N(50), S(120, LP_TOUCH=1, lpx=-20000, lpy=20000), S(120, LP_TOUCH=1, lpx=20000, lpy=20000),
               S(120, LP_TOUCH=1, lpx=20000, lpy=-20000), N(200)]


@case("pad_ring_center", "RP ring and centre sources in single-button mode")
def _():
    p = Profile(); p.analog(3, SINGLE, ring=8000); p.bind("RP_CENTER", key("c")); p.bind("RP_RING", key("r"))
    return p, [N(50), S(120, RP_TOUCH=1), S(120, RP_TOUCH=1, rpx=31000), S(120, RP_TOUCH=1), N(200)]


@case("pad_pressure", "LP D-pad activated by pressure")
def _():
    p = Profile(); p.analog(2, DPAD4, flags=AF_PRESS, dz_in=3000, press=5000); p.bind("LP_UP", key("w"))
    return p, [N(50), S(150, LP_TOUCH=1, lpy=25000, lpp=3000), S(150, LP_TOUCH=1, lpy=25000, lpp=25000), N(200)]


@case("mode_shift", "LB shifts the face buttons: A a -> A x, LS 8-way -> shifted keys")
def _():
    p = Profile(); p.bind("A", key("a")); p.shift(0, BTN["LB"]); p.bind("A", key("x"), shifted_index=SHIFT_SRC.index(0))
    p.analog(0, DPAD8, dz_in=3000); p.bind("LS_UP", key("w")); p.shift(2, BTN["LB"], mode=DPAD8, dz_in=3000)
    p.bind("LS_UP", key("i"), shifted_index=SHIFT_SRC.index(30))
    return p, [N(50), S(100, A=1), N(), S(100, LB=1), S(120, LB=1, A=1), S(100, LB=1), S(120, LB=1, lsy=28000),
               S(120, lsy=28000), N(), S(120, lsy=28000), N(200)]


@case("shifted_click", "LB shifts the left stick: L3 a, shifted L3 b")
def _():
    p = Profile(); p.bind("L3", key("a")); p.shift(2, BTN["LB"], mode=DPAD8, dz_in=3000)
    p.bind("L3", key("b"), shifted_click=True)
    return p, [N(50), S(100, L3=1), N(), S(100, LB=1), S(120, LB=1, L3=1), N(200)]


@case("passthrough_unbound", "unbound inputs do nothing: B pad passthrough off, A key")
def _():
    p = Profile(); p.flags(PF_BLOCK); p.bind("A", key("a"))
    return p, [N(50), S(120, A=1), N(), S(120, B=1), N(), S(120, X=1, Y=1), N(200)]


@case("radial_release_press", "LS radial with release press mode (the radial release bug)")
def _():
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=8)
    p.bind(30, key("w"), (), key("r"), mode=M_RELEASE)
    return p, [N(50), S(150, lsy=28000), N(300), S(150, lsy=28000), N(300)]


@case("turbo_with_hold", "A: tap a, hold turbo b")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_HOLD | M_TURBO); p.times(hold=250, turbo=60)
    return p, [N(50), S(80, A=1), N(300), S(700, A=1), N(300)]


_extra = {}
def build():
    out = []
    for name, what, f in CASES:
        _extra.clear()
        p, steps = f()
        out.append(dict(name=name, what=what, profile=bytes(p.b).hex(), script=script_bytes(steps).hex(),
                        ms=sum(ms for ms, _ in steps), extra={str(k): bytes(v.b).hex() for k, v in _extra.items()}))
    return out


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "self"
    if cmd == "export":
        cases = build()
        for c in cases: c["sim"] = sim_run(c)["log"]
        doc = dict(version=1, slot=SLOT, cfg=cfg_names(), cases=cases)
        path = os.path.join(HERE, "../../web/hwtests.json")
        json.dump(doc, open(path, "w"), separators=(",", ":"))
        print("wrote %s: %d cases" % (path, len(cases)))
    elif cmd == "self":
        bad = 0
        for c in build():
            r = sim_run(c)
            ok = r["flags"] & 8 == 0 and len(r["log"]) > 1
            bad += not ok
            print(("ok  " if ok else "BAD ") + "%-28s %3d entries%s" % (c["name"], len(r["log"]), " OVERFLOW" if r["flags"] & 8 else ""))
        sys.exit(1 if bad else 0)
    elif cmd == "check":
        res = json.load(open(sys.argv[2]))
        cases = {c["name"]: c for c in build()}
        nbad = 0
        for r in res["results"]:
            c = cases.get(r["name"])
            if c is None: print("?   %s (not in this version)" % r["name"]); continue
            if r.get("error"): print("ERR %-28s %s" % (r["name"], r["error"])); nbad += 1; continue
            sim = sim_run(c, res.get("cfg"))["log"]
            hw = parse_log(bytes.fromhex(r["log"]))
            probs = same(hw, sim)
            if r.get("flags", 0) & 16: probs.insert(0, "stopped early (Steam button or an upload)")
            nbad += bool(probs)
            print(("ok  " if not probs else "BAD ") + r["name"])
            for pr in probs: print("    " + pr)
        print("%d of %d differ" % (nbad, len(res["results"])))


if __name__ == "__main__":
    main()
