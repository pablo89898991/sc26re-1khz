"""Remapper hardware test: the same scripts on the real controller and in the simulator.

usage:  hwt.py export            write web/hwtests.json (profiles + scripts + what the simulator expects)
        hwt.py check RESULTS     compare a results file from web/hwtest.html with the simulator, and
                                 check what each case must do (its oracle) on the controller's log
        hwt.py self [NAME ...]   every case in the simulator through the firmware's own hwtest code,
                                 with its oracle

A case is a profile, a script of input frames (at most 32 steps, 10 s) and an oracle: what the
outputs must show. The firmware (hwtest.c, test builds) plays the script in its report loop and
records each change of the remapper's output (or, for analog cases, one snapshot per step); the
simulator runs the same hwtest.c around the same remap.c.
"""
import json, math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simlib import *  # noqa
import simlib
_key0 = simlib.key
def key(c):
    if len(c) == 1 and c.isdigit(): return 0x27 if c == "0" else 0x1E + int(c) - 1
    return _key0(c)

STEP = 42
MAX_STEPS = 32
ENTRY = 54
SLOT = 3   # the test profile goes to Profile 4 (the page backs it up and restores it)
LSLOT = 2  # layer / second profile for the cases that need one (Profile 3)


def cfg_names():
    import re
    src = open(os.path.join(HERE, "../../app/src/sc26re_cfg.c")).read()
    return re.findall(r'\[SC26RE_CFG_\w+\]\s*=\s*\{\s*"(\w+)"', src)


FIELDS = ("tl", "tr", "lsx", "lsy", "rsx", "rsy", "lpx", "lpy", "lpp", "rpx", "rpy", "rpp", "gx", "gy", "gz", "ax", "ay", "az")
def frame(st):
    b = st.get("buttons", 0)
    for k, v in st.items():
        if k in BTN and v: b |= 1 << BTN[k]
    vals = [int(st.get(k, 0)) for k in FIELDS]
    vals[8] &= 0xFFFF; vals[11] &= 0xFFFF
    return struct.pack("<I" + "h" * 8 + "H" + "hh" + "H" + "h" * 6, b, *vals)


def script_bytes(steps):
    assert 0 < len(steps) <= MAX_STEPS, len(steps)
    assert sum(ms for ms, _ in steps) <= 10000
    b = b"".join(struct.pack("<H", ms) + frame(st) for ms, st in steps)
    assert len(b) == STEP * len(steps)
    return b


def parse_log(raw):
    out = []
    for o in range(0, len(raw) - ENTRY + 1, ENTRY):
        e = raw[o:o + ENTRY]
        t, mb, sh = struct.unpack_from("<HBB", e)
        kbd = e[4:12]
        btn, xb, dx, dy, wh, xlt, xrt = struct.unpack_from("<IIiihBB", e, 12)
        xs = [v * 256 for v in struct.unpack_from("<4b", e, 32)]
        absx, absy = struct.unpack_from("<HH", e, 36)
        hap, prof = e[40], e[41]
        lsx, lsy, rsx, rsy, tl, tr = struct.unpack_from("<6h", e, 42)
        keys = sorted([0xE0 + i for i in range(8) if kbd[0] >> i & 1] + [c for c in kbd[2:] if c])
        out.append(dict(t=t, mb=mb, sh=sh, keys=keys, btn=btn, xb=xb, dx=dx, dy=dy, wh=wh, xlt=xlt, xrt=xrt, xs=list(xs),
                        hap=hap, prof=prof, absx=absx, absy=absy, lsx=lsx, lsy=lsy, rsx=rsx, rsy=rsy, tl=tl, tr=tr))
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
    p = Profile(); p.b[:] = bytes.fromhex(case["profile"])
    s.use(p, slot=case.get("slot", SLOT))
    s.lib.sim_xbox(1 if s.cfg_get("pad_out") else 0)  # Xbox actions go to the Xbox pad only while it is on
    s.run(50)
    sb = bytes.fromhex(case["script"])
    for o in range(0, len(sb), 40):
        assert s.feature(0xEE, "sc26re/hwt", b"\x01" + struct.pack("<H", o) + sb[o:o + 40])[0] == 0, o
    assert s.feature(0xEE, "sc26re/hwt", bytes([2, len(sb) // STEP, 1 if case.get("snap") else 0]))[0] == 0
    s.run(case["ms"] + 20)
    st = s.read("sc26re/hwt_st")
    n = struct.unpack_from("<H", st, 2)[0]
    s.feature(0xEE, "sc26re/hwt", b"\x04\x00\x00")
    raw = b""
    for i in range(n):
        d = s.read("sc26re/hwt_log"); assert struct.unpack_from("<H", d)[0] == i; raw += d[2:]
    s.feature(0xEE, "sc26re/hwt", b"\x03")
    return dict(flags=st[0], log=parse_log(raw))


# ---------------- comparison
DIG = lambda e: (e["mb"], e["sh"], tuple(e["keys"]), e["btn"], e["xb"], e["hap"], e["prof"])
AN = ("dx", "dy", "wh", "xlt", "xrt", "lsx", "lsy", "rsx", "rsy", "tl", "tr", "absx", "absy")

def same(a, b, tol_ms=6, snap=False, rel=0.06, blip=3, end=None):
    """a: controller log, b: simulator log. Same digital outputs in the same order, close in time;
    analog outputs close (per snapshot, or the totals at the end)."""
    probs = []
    if snap:
        if len(a) != len(b): return ["%d snapshots on the controller, %d in the simulator" % (len(a), len(b))]
        for i, (x, y) in enumerate(zip(a, b)):
            if DIG(x) != DIG(y): probs.append("step %d: controller %s, simulator %s" % (i, fmtx(DIG(x)), fmtx(DIG(y))))
            for k in AN:
                if abs(x[k] - y[k]) > max(3 if k in ("dx", "dy", "wh", "xlt", "xrt") else 400, rel * abs(y[k])):
                    probs.append("step %d %s: controller %d, simulator %d" % (i, k, x[k], y[k]))
            for j in range(4):
                if abs(x["xs"][j] - y["xs"][j]) > max(400, 0.06 * abs(y["xs"][j])):
                    probs.append("step %d Xbox stick %d: controller %d, simulator %d" % (i, j, x["xs"][j], y["xs"][j]))
        return probs[:8]
    if end is not None:
        # the script is over: the real sensors are back (a tilted desk, a drifting stick)
        a = [e for e in a if e["t"] < end] or a[:1]
    da, db = [DIG(e) for e in a], [DIG(e) for e in b]
    def ch(l, d):
        out = [(e["t"], x) for e, x, prev in zip(l, d, [None] + d[:-1]) if x != prev]
        # a state lasting under 3 ms at a step boundary (an input let go in the same millisecond
        # a turbo pulse starts) is not an output difference
        keep = [c for i, c in enumerate(out) if i + 1 >= len(out) or out[i + 1][0] - c[0] >= blip]
        merged = []
        for c in keep:
            if not merged or merged[-1][1] != c[1]: merged.append(c)
        return merged
    ca, cb = ch(a, da), ch(b, db)
    if [x for _, x in ca] != [x for _, x in cb]:
        probs.append("outputs differ:\n      controller: %s\n      simulator:  %s" % (fmt(ca), fmt(cb)))
    else:
        for (ta, x), (tb, _) in zip(ca, cb):
            if abs(ta - tb) > max(tol_ms, 0.05 * tb):
                probs.append("timing: %s at %d ms on the controller, %d ms in the simulator" % (fmtx(x), ta, tb))
    if a and b:
        for k in ("dx", "dy", "wh", "absx", "absy"):
            va, vb = a[-1][k], b[-1][k]
            if abs(va - vb) > max(3, 0.12 * abs(vb)):
                probs.append("mouse %s total: controller %d, simulator %d" % (k, va, vb))
    return probs[:8]


BN = {v: k for k, v in BTN.items()}
KN = {0x04 + i: chr(ord("a") + i) for i in range(26)}
KN.update({0x1E + i: str(i + 1) for i in range(9)}); KN[0x27] = "0"
KN.update({0x2C: "space", 0x28: "enter", 0x29: "esc", 0x2B: "tab", 0xE0: "lctrl", 0xE1: "lshift", 0xE2: "lalt"})

def fmtx(x):
    mb, sh, keys, btn, xb, hap, prof = x
    s = []
    if keys: s.append("keys " + "+".join(KN.get(k, "%02x" % k) for k in keys))
    if mb: s.append("mouse %x" % mb)
    if sh: s.append("shift %x" % sh)
    if btn: s.append("pad " + "+".join(BN.get(i, str(i)) for i in range(32) if btn >> i & 1))
    if xb: s.append("xbox " + "+".join(BN.get(i, str(i)) for i in range(32) if xb >> i & 1))
    if hap: s.append("haptics %d" % hap)
    s.append("profile %d" % (prof + 1))
    return ", ".join(s)

def fmt(c):
    return " | ".join("%d:%s" % (t, fmtx(x)) for t, x in c[:14]) + (" ..." if len(c) > 14 else "")


# ---------------- oracle helpers (on a parsed log)
def K(c): return key(c) if isinstance(c, str) else c
def taps(log, c):
    c = K(c); n, was = 0, False
    for e in log:
        now = c in e["keys"]
        if now and not was: n += 1
        was = now
    return n
def ever(log, c): return any(K(c) in e["keys"] for e in log)
def keys_at(log, t):
    cur = []
    for e in log:
        if e["t"] > t: break
        cur = e["keys"]
    return set(cur)
def first(log, c):
    for e in log:
        if K(c) in e["keys"]: return e["t"]
    return None
def edges(log, f, mask):
    n, was = 0, False
    for e in log:
        now = bool(e[f] & mask)
        if now and not was: n += 1
        was = now
    return n
def at(log, t):
    cur = log[0] if log else None
    for e in log:
        if e["t"] > t: break
        cur = e
    return cur
def need(cond, msg, probs):
    if not cond: probs.append(msg)


# ---------------- the cases
CASES = []
def case(name, what, tol_ms=6, snap=False, rel=0.06, slot=None, blip=3):
    """rel: how far apart analog totals may be (a trackball's speed depends on when the loop
    measures it against the 5 ms steps of the script)"""
    def deco(f):
        CASES.append(dict(name=name, what=what, f=f, tol=tol_ms, snap=snap, rel=rel, slot=SLOT if slot is None else slot, blip=blip)); return f
    return deco

def S(ms, **st): return (ms, st)
N = lambda ms=150: S(ms)
def tapb(b, ms=80, gap=150, **extra): return [S(ms, **{b: 1}, **extra), S(gap, **extra)]
def stick(side, deg, r, ms):
    a = math.radians(deg); x, y = int(r * math.cos(a)), int(r * math.sin(a))
    return S(ms, **({"lsx": x, "lsy": y} if side == 0 else {"rsx": x, "rsy": y}))
def pad(side, x, y, ms, click=False, p=0):
    k = "LP" if side == 0 else "RP"; f = "l" if side == 0 else "r"
    st = {k + "_TOUCH": 1, f + "px": x, f + "py": y}
    if click: st[k + "_CLICK"] = 1
    if p: st[f + "pp"] = p
    return S(ms, **st)

def ring_points(side, r, deg0, deg1, n, ms, click=False):
    out = []
    for i in range(n + 1):
        a = math.radians(deg0 + (deg1 - deg0) * i / n)
        out.append(pad(side, int(r * math.cos(a)), int(r * math.sin(a)), ms, click))
    return out

_extra = {}

# ===== actions =====
@case("act_keys_mods", "keys, modifiers, several keys on one input")
def _():
    p = Profile(); p.bind("A", key("a")); p.bind("B", key("lshift")); p.bind("X", (key("lctrl"), key("c"))); p.bind("Y", (key("1"), key("2"), key("3"), key("4")))
    def o(l, P):
        need(taps(l, "a") == 1 and taps(l, "lshift") == 1, "A -> a and B -> shift once each", P)
        need(any({key("lctrl"), key("c")} <= set(e["keys"]) for e in l), "X -> ctrl+c together", P)
        need(any({key("1"), key("2"), key("3"), key("4")} <= set(e["keys"]) for e in l), "Y -> 1+2+3+4 together", P)
        need(not l[-1]["keys"], "all released at the end", P)
    return p, [N(50)] + tapb("A") + tapb("B") + tapb("X") + tapb("Y") + [N(100)], o

@case("act_mouse_buttons", "mouse left, right, middle, back, forward")
def _():
    p = Profile(); p.bind("A", MOUSE_L); p.bind("B", MOUSE_R); p.bind("X", MOUSE_M); p.bind("Y", 0x7B); p.bind("LB", 0x7C)
    def o(l, P):
        for b, m in ((0, 1), (1, 2), (2, 4), (3, 8), (4, 16)):
            need(edges(l, "mb", m) == 1, "mouse button bit %d once" % b, P)
        need(l[-1]["mb"] == 0, "released", P)
    return p, [N(50)] + tapb("A") + tapb("B") + tapb("X") + tapb("Y") + tapb("LB") + [N(100)], o

@case("act_wheel_move", "wheel up / down / left / right, mouse move actions, move speed 2000")
def _():
    p = Profile(); p.bind("A", WHEEL_UP); p.bind("B", WHEEL_DOWN); p.bind("X", 0x74); p.bind("Y", 0x75)
    p.bind("DUP", MOVE_UP); p.bind("DRIGHT", MOVE_RIGHT); p.move_speed(2000)
    def o(l, P):
        need(l[-1]["wh"] == 0, "one notch up and one down: wheel total 0 (got %d)" % l[-1]["wh"], P)
        need(any(e["wh"] != 0 for e in l), "the wheel moved", P)
        need(800 <= l[-1]["dx"] <= 1200, "move right 0.5 s at 2000/s: ~1000 (got %d)" % l[-1]["dx"], P)
        need(-1200 <= l[-1]["dy"] <= -800, "move up 0.5 s: ~-1000 (got %d)" % l[-1]["dy"], P)
    return p, [N(50)] + tapb("A") + tapb("B") + tapb("X") + tapb("Y") + [S(500, DUP=1), N(), S(500, DRIGHT=1), N(100)], o

@case("act_pad_xbox_out", "pad buttons, Xbox buttons (to the Steam pad without the Xbox output), stick and trigger actions", snap=True)
def _():
    p = Profile(); p.flags(PF_BLOCK)
    p.bind("A", PAD + BTN["Y"]); p.bind("B", XBOX + BTN["X"]); p.bind("X", STICK + 0); p.bind("Y", STICK + 7)
    p.bind("LB", TRIG_L); p.bind("RB", XTRIG_R)
    def o(l, P):
        need(l[1]["btn"] & 1 << BTN["Y"], "A -> pad Y", P)
        need(l[3]["btn"] & 1 << BTN["X"] or l[3]["xb"] & 1 << BTN["X"], "B -> Xbox X (or the pad's X)", P)
        need(l[5]["lsy"] > 30000, "X -> left stick up full (lsy %d)" % l[5]["lsy"], P)
        need(l[7]["rsx"] > 30000, "Y -> right stick right full (rsx %d)" % l[7]["rsx"], P)
        need(l[9]["tl"] > 30000, "LB -> left trigger full (tl %d)" % l[9]["tl"], P)
        need(l[11]["tr"] > 30000 or l[11]["xrt"] > 250, "RB -> Xbox RT full", P)
        need(all(e["btn"] == 0 for e in l[::2]), "nothing on the pad between presses", P)
    return p, [N(50)] + [x for b in ("A", "B", "X", "Y", "LB", "RB") for x in (S(80, **{b: 1}), N(80))], o

@case("act_macro", "macro: a 50 ms, wait 100 ms, b 50 ms, mouse left in a macro; pressed again mid-macro")
def _():
    p = Profile(); p.macro(0, [(key("a"), 5), (0, 10), (key("b"), 5)]); p.bind("X", MACRO + 0)
    p.macro(3, [(MOUSE_L, 4), (0, 4), (MOUSE_L, 4)]); p.bind("Y", MACRO + 3)
    def o(l, P):
        need(taps(l, "a") >= 2 and taps(l, "b") >= 2, "the macro ran (a then b) at least twice", P)
        ta, tb = first(l, "a"), first(l, "b")
        need(ta is not None and tb is not None and 130 <= tb - ta <= 180, "b 150 ms after a (got %s)" % (None if ta is None or tb is None else tb - ta), P)
        need(edges(l, "mb", 1) == 2, "mouse macro clicks twice (got %d)" % edges(l, "mb", 1), P)
    return p, [N(50), S(40, X=1), N(500), S(40, X=1), N(60), S(40, X=1), N(400), S(40, Y=1), N(300)], o

@case("act_profile_switch", "L5 switches to Profile 3 (B = x there), its L5 switches back")
def _():
    p = Profile(); p.bind("B", key("b")); p.bind("L5", PROFILE + LSLOT)
    q = Profile("other"); q.bind("B", key("x")); q.bind("L5", PROFILE + SLOT); _extra[LSLOT] = q
    def o(l, P):
        need(taps(l, "b") == 2 and taps(l, "x") == 1, "b, then x in the other profile, then b again", P)
        need(any(e["prof"] == LSLOT for e in l) and l[-1]["prof"] == SLOT, "active profile went to 3 and back to 4", P)
        need(not any(e["btn"] & 1 << BTN["L5"] for e in l), "the switch button never reaches the Steam pad", P)
    return p, [N(50)] + tapb("B") + tapb("L5") + tapb("B") + tapb("L5") + tapb("B") + [N(100)], o

@case("act_layer", "L4 holds a layer (A = x, LS mouse there); release of the layer while A is held")
def _():
    p = Profile(); p.bind("A", key("a")); p.bind("L4", LAYER + LSLOT)
    lay = Profile("layer"); lay.bind("A", key("x")); lay.analog(0, MOUSE, speed=1000); _extra[LSLOT] = lay
    def o(l, P):
        need(taps(l, "x") == 2, "x twice with the layer (got %d)" % taps(l, "x"), P)
        need(taps(l, "a") == 3, "a: before, when the layer ends with A still held, and after (got %d)" % taps(l, "a"), P)
        need(l[-1]["dx"] > 100, "LS moves the mouse in the layer (dx %d)" % l[-1]["dx"], P)
        need(not l[-1]["keys"], "nothing stuck", P)
    return p, [N(50), S(100, A=1), N(100), S(100, L4=1), S(150, L4=1, A=1), S(100, L4=1), S(300, L4=1, lsx=32000),
               S(150, L4=1, A=1), S(150, A=1), N(100), S(100, A=1), N(150)], o

# ===== binding modes =====
@case("mode_hold_double", "A: tap a, hold b, double c (hold 300, double 250)")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), key("c"), mode=M_HOLD | M_DOUBLE); p.times(hold=300, dbl=250)
    def o(l, P):
        need(taps(l, "a") == 1 and taps(l, "b") == 1 and taps(l, "c") == 1, "one each of a, b, c (a %d b %d c %d)" % (taps(l, "a"), taps(l, "b"), taps(l, "c")), P)
    return p, [N(50), S(60, A=1), N(400), S(500, A=1), N(400), S(60, A=1), N(80), S(60, A=1), N(500)], o

@case("mode_toggle_start_release", "A toggle, B start press, X release press")
def _():
    p = Profile(); p.bind("A", key("a"), mode=M_TOGGLE); p.bind("B", key("b"), mode=M_START); p.bind("X", (), (), key("x"), mode=M_RELEASE)
    def o(l, P):
        need(taps(l, "a") == 1 and keys_at(l, 250) >= {key("a")} and key("a") not in keys_at(l, 700), "toggle on with the 1st press, off with the 2nd", P)
        tb = first(l, "b"); need(tb is not None and tb < 700 + 60, "start press: b at once", P)
        need(taps(l, "b") == 1 and taps(l, "x") == 1, "b once, x once", P)
        tx = first(l, "x"); need(tx is not None and tx >= 1450, "release press: x when released (at %s)" % tx, P)
    return p, [N(50), S(100, A=1), N(200), S(100, A=1), N(200), S(300, B=1), N(200), S(300, X=1), N(300)], o

@case("mode_turbo", "A turbo key, B turbo pad button, 50 ms")
def _():
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO); p.bind("B", PAD + BTN["A"], mode=M_TURBO); p.times(turbo=50)
    def o(l, P):
        n = taps(l, "a"); need(7 <= n <= 9, "400 ms at 50 ms: 8 pulses of a (got %d)" % n, P)
        m = edges(l, "btn", 1 << BTN["A"]); need(7 <= m <= 9, "8 pad A pulses (got %d)" % m, P)
    return p, [N(50), S(400, A=1), N(200), S(400, B=1), N(200)], o

@case("mode_turbo_haptic", "turbo with a haptic on every pulse (the turbo haptic bug)")
def _():
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO, haptic=2); p.times(turbo=100)
    def o(l, P):
        n, h = taps(l, "a"), l[-1]["hap"]
        need(n >= 5 and h == n, "a haptic for every pulse (%d pulses, %d haptics)" % (n, h), P)
    return p, [N(50), S(600, A=1), N(200)], o

@case("mode_cycle", "A cycles a, b, c")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), key("c"), mode=M_CYCLE)
    def o(l, P):
        seq = [k for e in l for k in e["keys"]]
        order = [k for i, k in enumerate(seq) if i == 0 or seq[i - 1] != k]
        need(order[:4] == [key("a"), key("b"), key("c"), key("a")], "a, b, c, a (got %s)" % [KN.get(k) for k in order[:4]], P)
    return p, [N(50)] + [x for _ in range(4) for x in (S(80, A=1), N(120))], o

@case("mode_chord", "B with RB held: b; B alone: nothing")
def _():
    p = Profile(); p.bind("B", key("b"), chord="RB")
    def o(l, P):
        need(taps(l, "b") == 1, "b only with RB (got %d)" % taps(l, "b"), P)
        need(first(l, "b") is not None and first(l, "b") >= 300, "not on B alone", P)
    return p, [N(50), S(100, B=1), N(150), S(100, RB=1), S(150, RB=1, B=1), S(100, RB=1), N(200)], o

@case("mode_hold_turbo", "A: tap a, hold turbo b")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_HOLD | M_TURBO); p.times(hold=250, turbo=60)
    def o(l, P):
        need(taps(l, "a") == 1, "tap -> a (got %d)" % taps(l, "a"), P)
        need(taps(l, "b") >= 3, "hold -> b repeating (got %d)" % taps(l, "b"), P)
    return p, [N(50), S(80, A=1), N(300), S(700, A=1), N(300)], o

@case("mode_hold_release", "A: hold b, release press r")
def _():
    p = Profile(); p.bind("A", key("a"), key("b"), key("r"), mode=M_HOLD | M_RELEASE); p.times(hold=250)
    def o(l, P):
        need(taps(l, "b") == 1, "b on the long press (got %d)" % taps(l, "b"), P)
        need(taps(l, "r") >= 1, "r on release (got %d)" % taps(l, "r"), P)
    return p, [N(50), S(80, A=1), N(300), S(500, A=1), N(300)], o

@case("btime_delay_interrupt", "per-binding times: delay start 100, delay end 200, interruptible hold")
def _():
    p = Profile(); p.bind("A", key("a")); p.btime("A", dstart=100)
    p.bind("B", key("b")); p.btime("B", dend=200)
    p.bind("X", key("x"), key("y"), mode=M_HOLD); p.btime("X", hold=300, interrupt=True)
    p.bind("Y", key("k"))
    def o(l, P):
        ta = first(l, "a"); need(ta is not None and 140 <= ta <= 170, "a 100 ms late (at %s, pressed at 50)" % ta, P)
        bs = [e["t"] for e in l if key("b") in e["keys"]]
        off = next((e["t"] for e in l if bs and e["t"] > bs[-1] and key("b") not in e["keys"]), None)
        need(off is not None and 620 <= off <= 650, "b lasts until 200 ms after release at 430 (off at %s)" % off, P)
        need(taps(l, "y") == 0, "the hold of X interrupted by Y: no y", P)
    return p, [N(50), S(150, A=1), N(130), S(100, B=1), N(400), S(100, X=1), S(300, X=1, Y=1), N(300)], o

@case("binding_haptics", "haptic levels 1, 2, 3 on presses")
def _():
    p = Profile(); p.bind("A", key("a"), haptic=1); p.bind("B", key("b"), haptic=2); p.bind("X", key("x"), haptic=3)
    def o(l, P): need(l[-1]["hap"] == 3, "3 haptics (got %d)" % l[-1]["hap"], P)
    return p, [N(50)] + tapb("A") + tapb("B") + tapb("X"), o

# ===== every button as a source =====
def _all_sources(lo, hi):
    p = Profile(); names = [n for n, b in sorted(BTN.items(), key=lambda kv: kv[1]) if lo <= b < hi and n != "STEAM"]
    codes = {}
    for i, n in enumerate(names):
        c = 0x04 + (i % 26); codes[n] = c; p.bind(n, c)
    steps = [N(40)] + [x for n in names for x in (S(60, **{n: 1}), N(60))]
    def o(l, P):
        seen = set(k for e in l for k in e["keys"])
        for n in names:
            need(codes[n] in seen, "%s -> its key" % n, P)
        need(all(len(e["keys"]) <= 1 for e in l), "never two keys at once", P)
    return p, steps, o

@case("sources_buttons_1", "every button 0..14 as a source with its own key")
def _(): return _all_sources(0, 15)
@case("sources_buttons_2", "every button 15..29 (not Steam) as a source with its own key")
def _(): return _all_sources(15, 30)

@case("physical_dpad_4way", "physical D-pad 4-way: a diagonal press gives one direction")
def _():
    p = Profile(); p.dpad4(1)
    for d, k in zip(("DUP", "DDOWN", "DLEFT", "DRIGHT"), "wsad"): p.bind(d, key(k))
    def o(l, P): need(all(len(e["keys"]) <= 1 for e in l), "never two directions", P); need(ever(l, "w") and ever(l, "d"), "up and right", P)
    return p, [N(50), S(150, DUP=1), S(150, DUP=1, DRIGHT=1), S(150, DRIGHT=1), N(150)], o

# ===== triggers =====
@case("trig_simple", "LT soft 30 % s, full f; RT full mouse right", snap=True)
def _():
    p = Profile(); p.trig_soft(0, 30); p.bind("LT_SOFT", key("s")); p.bind("LT_CLICK", key("f")); p.bind("RT_CLICK", MOUSE_R)
    vals = (3000, 8000, 12000, 20000, 32767, 12000, 8000, 1000)
    def o(l, P):
        ks = [set(e["keys"]) for e in l[1:9]]
        need(ks[0] == set() and ks[1] == set(), "below 30 %: nothing", P)
        need(key("s") in ks[2] and key("f") not in ks[2], "past 30 %: s", P)
        need({key("s"), key("f")} <= ks[4], "full + click: s and f", P)
        need(ks[7] == set(), "released: nothing", P)
        need(l[10]["mb"] & 2, "RT click -> mouse right", P)
    return p, [N(50)] + [S(80, tl=v, LT_CLICK=1 if v == 32767 else 0) for v in vals] + [N(80), S(120, tr=32767, RT_CLICK=1), N(100)], o

@case("trig_soft_3pct", "LT soft 3 %: on at 6 %, off again near 0 (the stuck soft pull bug)", snap=True)
def _():
    p = Profile(); p.trig_soft(0, 3); p.bind("LT_SOFT", key("s"))
    def o(l, P):
        need(key("s") in l[1]["keys"], "2000 (6 %): on", P)
        need(key("s") not in l[3]["keys"], "300: off", P)
        need(key("s") not in l[-1]["keys"], "off at the end", P)
    return p, [N(50), S(100, tl=2000), S(100, tl=800), S(100, tl=300), N(150)], o

def _trig_mode(mode, slow):
    p = Profile(); p.trig_soft(1, 30); p.trig_mode(1, mode); p.bind("RT_SOFT", key("s")); p.bind("RT_CLICK", key("f"))
    if slow: st = [N(50), S(150, tr=12000), S(150, tr=20000), S(200, tr=32767, RT_CLICK=1), N(200)]
    else: st = [N(50), S(10, tr=16000), S(250, tr=32767, RT_CLICK=1), N(200)]
    return p, st

@case("trig_hair", "RT hair trigger: soft on any further pull, off on any release")
def _():
    p = Profile(); p.trig_soft(1, 30); p.trig_mode(1, 1); p.bind("RT_SOFT", key("s"))
    def o(l, P): need(taps(l, "s") >= 2, "pull, ease off, pull again: s twice (got %d)" % taps(l, "s"), P)
    return p, [N(50), S(100, tr=14000), S(100, tr=20000), S(100, tr=17000), S(100, tr=22000), N(150)], o

@case("trig_hip_quick", "RT hip fire, quick full pull: only f")
def _():
    p, st = _trig_mode(2, False)
    def o(l, P): need(taps(l, "f") == 1 and taps(l, "s") == 0, "only f (s %d f %d)" % (taps(l, "s"), taps(l, "f")), P)
    return p, st, o

@case("trig_hip_slow", "RT hip fire, slow pull: s, then f too")
def _():
    p, st = _trig_mode(2, True)
    def o(l, P): need(taps(l, "s") == 1 and taps(l, "f") == 1, "s then f (s %d f %d)" % (taps(l, "s"), taps(l, "f")), P)
    return p, st, o

@case("trig_hip_excl", "RT hip fire exclusive, slow pull: s then f, never both")
def _():
    p, st = _trig_mode(3, True)
    def o(l, P):
        need(not any({key("s"), key("f")} <= set(e["keys"]) for e in l), "never s and f together", P)
        need(ever(l, "s") and ever(l, "f"), "both happened", P)
    return p, st, o

@case("trig_range_curve", "RT out range 20..80 %, LT curve 2.0", snap=True)
def _():
    p = Profile(); struct.pack_into("<HH", p.b, 114, 2000, 8000); p.trig_curve(0, 200)
    vals = (3000, 16384, 29000)
    def o(l, P):
        need(l[1]["tr"] == 0 and l[3]["tr"] == 32767 and 14000 < l[2]["tr"] < 19000, "RT 20..80 %% range (%d, %d, %d)" % (l[1]["tr"], l[2]["tr"], l[3]["tr"]), P)
        need(6000 < l[5]["tl"] < 10500, "LT half pull squared ~ quarter (got %d)" % l[5]["tl"], P)
    return p, [N(50)] + [S(60, tr=v) for v in vals] + [N(60), S(60, tl=16384), N(60)], o

@case("trig_xbox_whole", "LT whole trigger to Xbox LT (to the Steam pad's LT without the Xbox output)", snap=True)
def _():
    p = Profile(); p.flags(PF_BLOCK); p.bind("LT_SOFT", XTRIG_L)
    def o(l, P): need(l[2]["tl"] > 12000 or l[2]["xlt"] > 90, "half pull reaches the LT output (tl %d, xlt %d)" % (l[2]["tl"], l[2]["xlt"]), P)
    return p, [N(50), S(60, tl=8000), S(60, tl=16384), S(60, tl=32767), N(60)], o

# ===== sticks =====
def _round(side, step, r=28000, ms=50, start=90):
    return [stick(side, d, r, ms) for d in range(start, start + 361, step)]

@case("stick_dpad4_round", "LS 4-way wasd, rolling round: always one key")
def _():
    p = Profile(); p.analog(0, DPAD4, dz_in=3000)
    for d, k in zip(("LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT"), "wsad"): p.bind(d, key(k))
    def o(l, P):
        need(all(len(e["keys"]) <= 1 for e in l), "one direction at a time", P)
        need(all(ever(l, c) for c in "wsad"), "all four", P)
    return p, [N(50)] + _round(0, 30, ms=60) + [N(150)], o

@case("stick_dpad8_round", "LS 8-way wasd, rolling round: diagonals hold two")
def _():
    p = Profile(); p.analog(0, DPAD8, dz_in=3000)
    for d, k in zip(("LS_UP", "LS_DOWN", "LS_LEFT", "LS_RIGHT"), "wsad"): p.bind(d, key(k))
    def o(l, P):
        need(any({key("w"), key("d")} <= set(e["keys"]) for e in l), "up-right = w + d", P)
        need(all(ever(l, c) for c in "wsad"), "all four", P)
    return p, [N(50)] + _round(0, 22, ms=50) + [N(150)], o

def _ring_case(side, mode, inv, ring=8000, rs=(20000, 31000)):
    p = Profile(); p.analog(side, mode, dz_in=3000, ring=ring, flags=AF_RING_INV if inv else 0)
    pre = "LS" if side == 0 else "RS"
    for d, k in zip(("_UP", "_DOWN", "_LEFT", "_RIGHT"), "wsad"): p.bind(pre + d, key(k))
    p.bind(pre + "_RING", key("lshift"))
    st = [N(50)]
    for r in rs:
        st += [stick(side, d, r, 60) for d in range(90, 90 + 361, 45)] + [N(80)]
    t_in = (50, 50 + 9 * 60); t_out = (50 + 9 * 60 + 80, 50 + 18 * 60 + 80)
    def o(l, P):
        inner = [e for e in l if t_in[0] + 10 <= e["t"] < t_in[1]]; outer = [e for e in l if t_out[0] + 10 <= e["t"] < t_out[1]]
        shift_in = any(0xE1 in e["keys"] for e in inner)
        shift_out = bool(outer) and all(0xE1 in e["keys"] for e in outer)
        if inv: need(shift_in and not any(0xE1 in e["keys"] for e in outer), "inverted ring: shift inside only", P)
        else: need(not shift_in and shift_out, "ring: shift only outside, all the way round", P)
        dirs = set(tuple(k for k in e["keys"] if k != 0xE1) for e in outer)
        need(len(dirs) >= (8 if mode == DPAD8 else 4), "directions keep changing on the outer circle (%d different)" % len(dirs), P)
        dirs_in = set(tuple(k for k in e["keys"] if k != 0xE1) for e in inner)
        need(len(dirs_in) >= (8 if mode == DPAD8 else 4), "directions keep changing on the inner circle (%d different)" % len(dirs_in), P)
        need(not l[-1]["keys"], "released", P)
    return p, st + [N(100)], o

@case("stick_dpad8_outer_ring", "LS 8-way + outer ring: directions change all the way round, inside and outside")
def _(): return _ring_case(0, DPAD8, False)
@case("stick_dpad4_outer_ring", "LS 4-way + outer ring")
def _(): return _ring_case(0, DPAD4, False)
@case("stick_dpad8_inner_ring", "LS 8-way + inverted ring")
def _(): return _ring_case(0, DPAD8, True)
@case("rstick_dpad8_outer_ring_90", "RS 8-way + outer ring at 90 %")
def _(): return _ring_case(1, DPAD8, False, ring=9000, rs=(20000, 32000))

def _ring_first_case(side, mode):
    """outer ring "ring key first" (v51 test 83): reaching the ring lets the direction go for
    one pass, so the ring key (Shift) is down before the direction comes back"""
    p = Profile(); p.analog(side, mode, dz_in=800, ring=8600, flags=0x08)
    pre = "LS" if side == 0 else "RS"
    for d, k in zip(("_UP", "_DOWN", "_LEFT", "_RIGHT"), "wsad"): p.bind(pre + d, key(k))
    p.bind(pre + "_RING", key("lshift"))
    st = [N(50), stick(side, 90, 16000, 60), stick(side, 90, 32767, 120), N(120), stick(side, 0, 32767, 120), N(120),
          stick(side, 180, 25000, 60), stick(side, 180, 32767, 80), stick(side, 135, 32767, 80), N(120)]
    sh = 0xE1
    def o(l, P):
        def before_ring(k):
            ks = [e["keys"] for e in l]
            for i, x in enumerate(ks):
                if sh in x:
                    return k not in x
            return False
        need(before_ring(key("w")), "walking up into the ring: Shift alone before W comes back", P)
        need(any({sh, key("w")} <= set(e["keys"]) for e in l), "then W + Shift", P)
        right = [e for e in l if 50 + 60 + 120 + 120 <= e["t"] < 50 + 60 + 120 + 120 + 120]
        need(right and right[0]["keys"] and key("d") not in right[0]["keys"], "straight to the edge: Shift before D", P)
        need(any({sh, key("d")} <= set(e["keys"]) for e in right), "then D + Shift", P)
        diag = {sh, key("a"), key("w")} if mode == DPAD8 else {sh, key("a")}
        need(any(diag <= set(e["keys"]) for e in l), "turning along the ring keeps Shift", P)
        need(not l[-1]["keys"], "released", P)
    return p, st + [N(100)], o

@case("stick_dpad8_ring_first", "LS 8-way + outer ring, ring key first: Shift before W", blip=1)
def _(): return _ring_first_case(0, DPAD8)
@case("stick_dpad4_ring_first", "LS 4-way + outer ring, ring key first", blip=1)
def _(): return _ring_first_case(0, DPAD4)

@case("stick_pass_shaping", "LS to Steam: dead zones, anti dead zone, invert; RS curve 2.0 + rotate 90", snap=True)
def _():
    p = Profile(); p.analog(0, PASS, dz_in=1000, dz_out=1000, anti=2000, flags=AF_INVY); p.analog(1, PASS, curve=200, rotate=90)
    def o(l, P):
        need(l[1]["lsx"] == 0, "inside the dead zone: 0 (got %d)" % l[1]["lsx"], P)
        need(6500 <= l[2]["lsx"] <= 8500, "anti dead zone kicks in (got %d)" % l[2]["lsx"], P)
        need(l[3]["lsx"] == 32767, "outer dead zone: full (got %d)" % l[3]["lsx"], P)
        need(l[4]["lsy"] < -15000, "invert y (got %d)" % l[4]["lsy"], P)
        need(abs(l[5]["rsx"]) > 3000 and abs(l[5]["rsy"]) < 1500, "rotate 90: up becomes sideways (%d, %d)" % (l[5]["rsx"], l[5]["rsy"]), P)
    return p, [N(50), S(60, lsx=2500), S(60, lsx=4000), S(60, lsx=31000), S(60, lsy=20000), S(60, rsy=16384), N(60)], o

@case("stick_mouse", "RS mouse 1000/s, scale y 200 %")
def _():
    p = Profile(); p.analog(1, MOUSE, speed=1000); p.anx(1, scale_y=200)
    def o(l, P):
        need(400 <= l[-1]["dx"] <= 600, "0.5 s full right ~500 (got %d)" % l[-1]["dx"], P)
        need(l[-1]["dy"] < -500, "y doubled: up 0.5 s ~ -1000 (got %d)" % l[-1]["dy"], P)
    return p, [N(50), S(500, rsx=32767), N(100), S(500, rsy=32767), N(150)], o

@case("stick_scroll", "RS scroll up then down")
def _():
    p = Profile(); p.analog(1, SCROLL, speed=1000)
    def o(l, P):
        mx = max(e["wh"] for e in l); need(mx >= 5, "scrolled up (max %d)" % mx, P); need(abs(l[-1]["wh"]) <= 1, "and back down (end %d)" % l[-1]["wh"], P)
    return p, [N(50), S(800, rsy=32767), N(100), S(800, rsy=-32767), N(200)], o

@case("stick_flick", "RS flick stick: point right = quarter turn")
def _():
    p = Profile(); p.analog(1, FLICK, speed=100)
    def o(l, P): need(3700 <= l[-1]["dx"] <= 4500, "quarter turn ~4091 (got %d)" % l[-1]["dx"], P)
    return p, [N(50), S(300, rsx=32767), N(300)], o

@case("stick_radial", "LS radial 8 slices: fires on release")
def _():
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=8)
    for i, k in enumerate("12345qwe"): p.bind((30, 70, 33, 71, 31, 72, 32, 73)[i], key(k))
    def o(l, P):
        need(taps(l, "3") == 1, "right slice fires once (3: %d)" % taps(l, "3"), P)
        need(taps(l, "w") == 1, "the last slice it rested on (left) fires", P)
        need(not keys_at(l, 190), "nothing while choosing", P)
    return p, [N(50), S(150, lsx=28000), N(200), S(150, lsy=28000), S(150, lsx=-28000), N(200)], o

@case("stick_radial_release_press", "LS radial with release press mode (the radial release bug)")
def _():
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=8); p.bind(30, key("w"), (), key("r"), mode=M_RELEASE)
    def o(l, P): need(taps(l, "r") == 2, "r on each release (got %d)" % taps(l, "r"), P)
    return p, [N(50), S(150, lsy=28000), N(300), S(150, lsy=28000), N(300)], o

@case("stick_region", "RS mouse region (absolute): far right")
def _():
    p = Profile(); p.analog(1, REGION, dz_in=1000); p.screen(1000, 500)
    def o(l, P):
        r = at(l, 240); need(r is not None and r["absx"] != 0xFFFF and r["absx"] > 30000, "far right: the cursor at the right edge (%s)" % (r and r["absx"]), P)
        need(any(e["t"] >= 245 and abs(e["absx"] - 16383) < 300 for e in l), "let go: back to the centre", P)
    return p, [N(50), S(200, rsx=32767), S(200), N(100)], o

@case("stick_xbox_mode", "LS to the Xbox stick (Steam pad stick without the Xbox output)", snap=True)
def _():
    p = Profile(); p.analog(0, XSTICK, dz_in=1000)
    def o(l, P): need(l[1]["lsx"] > 20000 or l[1]["xs"][0] > 20000, "full right reaches the stick output", P)
    return p, [N(50), S(60, lsx=32000), N(60)], o

# ===== trackpads =====
@case("pad_dpad_touch_click", "LP D-pad by touch, RP D-pad by click (click owned)")
def _():
    p = Profile(); p.analog(2, DPAD4, dz_in=3000); p.analog(3, DPAD4, dz_in=3000, flags=AF_CLICK)
    for d, k in zip(("LP_UP", "LP_DOWN", "LP_LEFT", "LP_RIGHT"), "wsad"): p.bind(d, key(k))
    for d, k in zip(("RP_UP", "RP_DOWN", "RP_LEFT", "RP_RIGHT"), "ikjl"): p.bind(d, key(k))
    p.bind("RP_CLICK", key("space"))
    def o(l, P):
        need(ever(l, "w") and ever(l, "d"), "LP touch: w, d", P)
        need(taps(l, "i") == 1 and taps(l, "j") == 1, "RP click: i, j", P)
        need(not ever(l, "space"), "the RP click itself is owned by the D-pad", P)
        need(first(l, "i") is not None and first(l, "i") >= 560, "RP touch alone: nothing", P)
    return p, [N(50), pad(0, 0, 25000, 120), pad(0, 25000, 0, 120), N(), pad(1, 0, 25000, 120), pad(1, 0, 25000, 120, True), N(),
               pad(1, -25000, 0, 120, True), N(200)], o

@case("pad_dpad8_pressure", "LP 8-way by pressure")
def _():
    p = Profile(); p.analog(2, DPAD8, flags=AF_PRESS, dz_in=3000, press=5000); p.bind("LP_UP", key("w")); p.bind("LP_RIGHT", key("d"))
    def o(l, P):
        need(ever(l, "w") and first(l, "w") >= 200, "only when pressed hard", P)
        need(any({key("w"), key("d")} <= set(e["keys"]) for e in l), "diagonal w+d", P)
    return p, [N(50), pad(0, 0, 25000, 150, p=3000), pad(0, 0, 25000, 150, p=25000), pad(0, 20000, 20000, 150, p=25000), N(200)], o

@case("pad_grid_click", "RP 9-zone grid with click: zone keys, the click not on top (the grid click bug)")
def _():
    p = Profile(); p.analog(3, GRID, flags=AF_CLICK)
    p.bind("RP_CENTER", key("c")); p.bind("RP_UP", key("w")); p.bind("RP_G1", key("e")); p.bind("RP_CLICK", key("space"))
    def o(l, P):
        need(taps(l, "c") == 1 and taps(l, "w") == 1 and taps(l, "e") == 1, "c, w, e once each", P)
        need(not ever(l, "space"), "no click binding on top", P)
    return p, [N(50), pad(1, 0, 0, 120), pad(1, 0, 0, 120, True), N(), pad(1, 0, 28000, 120, True), N(), pad(1, 28000, 28000, 120, True), N(200)], o

@case("pad_grid_lines", "LP grid with custom lines (centre zone 80 % wide)")
def _():
    p = Profile(); p.analog(2, GRID); struct.pack_into("<4h", p.b, 22, -8000, 8000, 8000, -8000)
    p.bind("LP_CENTER", key("c")); p.bind("LP_RIGHT", key("d"))
    def o(l, P): need(first(l, "c") is not None and first(l, "c") < 200 and taps(l, "d") == 1, "58 % right is still the centre, 92 % is right", P)
    return p, [N(50), pad(0, 19000, 0, 150), N(), pad(0, 30000, 0, 150), N(150)], o

@case("pad_radial", "RP radial: slide to a slice, fires on lift; lift in the centre cancels")
def _():
    p = Profile(); p.analog(3, RADIAL, speed=4, dz_in=2500)
    for i, k in enumerate("1234"): p.bind((42, 51, 45, 53)[i], key(k))
    def o(l, P): need(sum(taps(l, k) for k in "1234") == 1, "exactly one slice fired", P)
    return p, [N(50), pad(1, 0, 0, 60), pad(1, 25000, 0, 120), N(200), pad(1, 0, 0, 60), N(200)], o

@case("pad_scroll_wheel_cw", "LP circle scroll clockwise: q", tol_ms=40)
def _():
    p = Profile(); p.analog(2, SCROLL, speed=30); p.bind("LP_CW", key("q")); p.bind("LP_CCW", key("e"))
    def o(l, P): need(taps(l, "q") >= 8 and taps(l, "e") == 0, "a turn clockwise: q %d, e %d" % (taps(l, "q"), taps(l, "e")), P)
    return p, [N(50)] + ring_points(0, 25000, 90, -270, 16, 25) + [N(500)], o

@case("pad_scroll_wheel_ccw", "RP circle scroll counter-clockwise: e", tol_ms=40)
def _():
    p = Profile(); p.analog(3, SCROLL, speed=30); p.bind("RP_CW", key("q")); p.bind("RP_CCW", key("e"))
    def o(l, P): need(taps(l, "e") >= 8 and taps(l, "q") == 0, "a turn counter-clockwise: e %d, q %d" % (taps(l, "e"), taps(l, "q")), P)
    return p, [N(50)] + ring_points(1, 25000, 90, 450, 16, 25) + [N(500)], o

@case("pad_mouse", "RP mouse: a slide moves, nothing after lifting")
def _():
    p = Profile(); p.analog(3, MOUSE, speed=100)
    st = [N(50)] + [pad(1, -15000 + i * 1200, 0, 8) for i in range(25)] + [N(300)]
    def o(l, P):
        need(l[-1]["dx"] > 20, "moved right (dx %d)" % l[-1]["dx"], P)
        lift = at(l, 50 + 25 * 8 + 20); need(lift is None or l[-1]["dx"] - lift["dx"] <= 2, "stops when lifted", P)
    return p, st, o

@case("pad_trackball", "RP mouse with trackball: keeps rolling after lifting")
def _():
    p = Profile(); p.analog(3, MOUSE, flags=AF_TRACKBALL, speed=100, dz_out=200)
    st = [N(50)] + [pad(1, -15000 + i * 1500, 0, 5) for i in range(20)] + [N(1500)]
    def o(l, P):
        lift = at(l, 50 + 20 * 5 + 10); need(lift is not None and l[-1]["dx"] - lift["dx"] > 5, "rolled on after lifting", P)
    return p, st, o

@case("pad_swipe", "RP swipe right d, left a, repeat off")
def _():
    p = Profile(); p.analog(3, SWIPE, speed=3000); p.bind("RP_RIGHT", key("d")); p.bind("RP_LEFT", key("a"))
    def o(l, P): need(taps(l, "d") == 1 and taps(l, "a") == 1, "one d, one a (d %d a %d)" % (taps(l, "d"), taps(l, "a")), P)
    return p, ([N(50)] + [pad(1, -15000 + i * 2500, 0, 8) for i in range(13)] + [N(200)]
               + [pad(1, 15000 - i * 2500, 0, 8) for i in range(13)] + [N(200)]), o

@case("pad_gates", "LP 4 gates with zone haptics")
def _():
    p = Profile(); p.analog(2, GATES, dz_in=2000, haptic=1)
    for i, k in enumerate("qepz"): p.bind(46 + i, key(k))
    def o(l, P): need(ever(l, "q") and ever(l, "e") and ever(l, "z"), "q, e, z", P); need(l[-1]["hap"] >= 3, "a tick per gate (%d)" % l[-1]["hap"], P)
    return p, [N(50), pad(0, -20000, 20000, 120), pad(0, 20000, 20000, 120), pad(0, 20000, -20000, 120), N(200)], o

@case("pad_single_ring", "RP single button + ring: centre c, ring r")
def _():
    p = Profile(); p.analog(3, SINGLE, ring=8000); p.bind("RP_CENTER", key("c")); p.bind("RP_RING", key("r"))
    def o(l, P): need(ever(l, "c") and ever(l, "r"), "c and r", P)
    return p, [N(50), pad(1, 0, 0, 120), pad(1, 31000, 0, 120), pad(1, 0, 0, 120), N(200)], o

@case("pad_joystick_other", "RP joystick to the left stick (other stick)", snap=True)
def _():
    p = Profile(); p.analog(3, JOYSTICK, flags=AF_OTHER, dz_in=500)
    def o(l, P): need(l[1]["lsx"] > 10000, "finger right -> left stick right (lsx %d)" % l[1]["lsx"], P)
    return p, [N(50), pad(1, 16000, 0, 80), N(80)], o

@case("pad_region_return", "LP mouse region with return to centre")
def _():
    p = Profile(); p.analog(2, REGION); p.anx(2, flags=1)
    def o(l, P):
        r = at(l, 240); need(r is not None and 23000 < r["absx"] < 27000, "finger at 61 %% right: cursor ~80 %% (%s)" % (r and r["absx"]), P)
        need(any(e["t"] >= 245 and abs(e["absx"] - 16383) < 300 for e in l), "back to the centre when lifted", P)
    return p, [N(50), pad(0, 20000, 0, 200), N(300)], o

# ===== mode shift =====
@case("shift_face_and_stick", "LB shifts face buttons (A a -> x) and LS (8-way, w -> i)")
def _():
    p = Profile(); p.bind("A", key("a")); p.shift(0, BTN["LB"]); p.bind("A", key("x"), shifted_index=SHIFT_SRC.index(0))
    p.analog(0, DPAD8, dz_in=3000); p.bind("LS_UP", key("w")); p.shift(2, BTN["LB"], mode=DPAD8, dz_in=3000)
    p.bind("LS_UP", key("i"), shifted_index=SHIFT_SRC.index(30))
    def o(l, P):
        need(taps(l, "a") == 1 and taps(l, "x") == 1 and ever(l, "i") and ever(l, "w"), "a, x with LB, i with LB, w without", P)
        need(any(e["sh"] for e in l), "the shift shows", P)
    return p, [N(50), S(100, A=1), N(), S(100, LB=1), S(120, LB=1, A=1), S(100, LB=1), S(120, LB=1, lsy=28000), S(120, lsy=28000), N(), S(120, lsy=28000), N(200)], o

@case("shift_stick_to_mouse", "R4 shifts RS from 4-way to mouse")
def _():
    p = Profile(); p.analog(1, DPAD4, dz_in=3000); p.bind("RS_RIGHT", key("l")); p.shift(3, BTN["R4"], mode=MOUSE, speed=1000)
    def o(l, P): need(ever(l, "l"), "unshifted: l", P); need(l[-1]["dx"] > 150, "shifted: the mouse moves (dx %d)" % l[-1]["dx"], P)
    return p, [N(50), S(200, rsx=32000), N(), S(50, R4=1), S(300, R4=1, rsx=32000), N(150)], o

@case("shift_more_groups", "D-pad, LT and RT shift groups with their own buttons")
def _():
    p = Profile()
    p.bind("DUP", key("u")); p.shift(1, BTN["L4"]); p.bind("DUP", key("1"), shifted_index=SHIFT_SRC.index(13))
    p.bind("LT_CLICK", key("t")); p.shift(6, BTN["L5"]); p.bind("LT_CLICK", key("2"), shifted_index=SHIFT_SRC.index(27))
    p.bind("RT_CLICK", key("y")); p.shift(7, BTN["R5"]); p.bind("RT_CLICK", key("3"), shifted_index=SHIFT_SRC.index(23))
    def o(l, P):
        for a, b in (("u", "1"), ("t", "2"), ("y", "3")): need(taps(l, a) == 1 and taps(l, b) == 1, "%s, then %s shifted" % (a, b), P)
    st = [N(50)]
    for sb, src, kw in (("L4", "DUP", {}), ("L5", "LT_CLICK", {"tl": 32767}), ("R5", "RT_CLICK", {"tr": 32767})):
        st += [S(80, **{src: 1}, **kw), N(80), S(60, **{sb: 1}), S(80, **{sb: 1, src: 1}, **kw), N(80)]
    return p, st, o

@case("shift_clicks", "LB shifts LS and RP: L3 a -> b, RP click c -> d")
def _():
    p = Profile(); p.bind("L3", key("a")); p.shift(2, BTN["LB"], mode=DPAD8, dz_in=3000); p.bind("L3", key("b"), shifted_click=True)
    p.bind("RP_CLICK", key("c")); p.shift(5, BTN["LB"]); p.bind("RP_CLICK", key("d"), shifted_click=True)
    def o(l, P): need(taps(l, "a") == 1 and taps(l, "b") == 1 and taps(l, "c") == 1 and taps(l, "d") == 1, "a b c d once each", P)
    return p, [N(50), S(100, L3=1), N(), S(100, RP_CLICK=1, RP_TOUCH=1), N(), S(80, LB=1), S(120, LB=1, L3=1), S(80, LB=1),
               S(120, LB=1, RP_CLICK=1, RP_TOUCH=1), N(200)], o

# ===== profile flags =====
@case("block_unbound", "unbound inputs do nothing: B, X, Y, sticks and triggers silent; A key", snap=True)
def _():
    p = Profile(); p.flags(PF_BLOCK); p.bind("A", key("a"))
    def o(l, P):
        need(key("a") in l[1]["keys"], "A -> a", P)
        need(all(e["btn"] == 0 for e in l), "no pad buttons", P)
        need(all(e["tl"] == 0 for e in l), "no trigger", P)
        need(l[4]["lsx"] > 25000, "a stick in Analog mode still works (lsx %d)" % l[4]["lsx"], P)
    return p, [N(50), S(100, A=1), S(100, B=1), S(100, X=1, Y=1), S(100, lsx=30000), S(100, tl=30000), N(60)], o

@case("unbound_pass", "without the flag unbound inputs reach the Steam pad, bound ones do not", snap=True)
def _():
    p = Profile(); p.bind("A", key("a"))
    def o(l, P):
        need(l[1]["btn"] & 1 << BTN["A"] == 0, "bound A hidden from the pad", P)
        need(l[2]["btn"] & 1 << BTN["B"], "unbound B to the pad", P)
        need(l[3]["lsx"] > 25000, "unbound stick to the pad (lsx %d)" % l[3]["lsx"], P)
    return p, [N(50), S(100, A=1), S(100, B=1), S(100, lsx=30000), N(60)], o

# ===== gyro =====
def _gyro_case(q, tilts):
    """the gyro profile is switched to during the script (L5), so its tilt centre is the script's
    own position, not how the controller lies on the table"""
    p = Profile("gyro base"); p.bind("L5", PROFILE + LSLOT); _extra[LSLOT] = q
    g = 14000
    def acc(fwd, side):
        a, b = math.radians(fwd), math.radians(side)
        y, z = -g * math.sin(a), g * math.cos(a)
        x, z = -z * math.sin(b), z * math.cos(b)
        return dict(ax=int(x), ay=int(y), az=int(z))
    flat = acc(0, 0)
    return [S(100, **flat), S(80, L5=1, **flat), S(150, **flat)] + [S(ms, **acc(f, sd)) for f, sd, ms in tilts]

@case("gyro_tilt", "gyro tilt 4-way: forward w, sideways d / a")
def _():
    q = Profile("tilt"); q.gyx(mode=1, tilt_dz=15); q.bind("GYRO_UP", key("w")); q.bind("GYRO_RIGHT", key("d")); q.bind("GYRO_LEFT", key("a"))
    st = _gyro_case(q, [(25, 0, 300), (0, 0, 300), (0, 25, 300), (0, -25, 300), (10, 0, 300), (0, 0, 200)])
    def o(l, P):
        need(ever(l, "w") and ever(l, "d") and ever(l, "a"), "w, d and a (w %s d %s a %s)" % (ever(l, "w"), ever(l, "d"), ever(l, "a")), P)
        need(not keys_at(l, 330 + 4 * 300 + 150), "10 degrees: inside the dead zone", P)
    return _gp(st, o)

@case("gyro_tilt_8way", "gyro tilt 8-way: a diagonal tilt holds two")
def _():
    q = Profile("tilt8"); q.gyx(mode=1, tilt_dz=15, tilt_8way=1); q.bind("GYRO_UP", key("w")); q.bind("GYRO_RIGHT", key("d")); q.bind("GYRO_LEFT", key("a"))
    st = _gyro_case(q, [(30, 25, 300), (0, 0, 300), (30, -25, 300), (0, 0, 200)])
    def o(l, P): need(any(len(e["keys"]) == 2 and key("w") in e["keys"] for e in l), "w plus a side on a diagonal tilt", P)
    return _gp(st, o)

@case("gyro_region", "gyro mouse region: turning moves the cursor")
def _():
    q = Profile("region"); q.gyx(mode=2, region_deg=60)
    st = _gyro_case(q, [])
    st += [S(300, az=14000, gz=-500), S(300, az=14000), S(300, az=14000, gz=500), S(100, az=14000)]
    def o(l, P):
        xs = [e["absx"] for e in l if e["absx"] != 0xFFFF]
        need(xs and max(xs) - min(xs) > 3000, "turning moves the cursor (x %s..%s)" % (min(xs, default=None), max(xs, default=None)), P)
    return _gp(st, o)

def _gp(st, o):
    p = Profile("gyro base"); p.bind("L5", PROFILE + LSLOT)
    return p, st, o

# ===== more combinations =====
@case("mode_start_release", "A: start press a, release press r")
def _():
    p = Profile(); p.bind("A", key("a"), (), key("r"), mode=M_START | M_RELEASE)
    def o(l, P):
        need(taps(l, "a") == 2 and taps(l, "r") == 2, "a at each press, r at each release (a %d r %d)" % (taps(l, "a"), taps(l, "r")), P)
        need(first(l, "a") < 70 and first(l, "r") >= 450, "a at once, r on release", P)
    return p, [N(50), S(400, A=1), N(200), S(100, A=1), N(200)], o

@case("mode_toggle_turbo", "A toggle + turbo: pulses until pressed again")
def _():
    p = Profile(); p.bind("A", key("a"), mode=M_TOGGLE | M_TURBO); p.times(turbo=60)
    def o(l, P):
        n = taps(l, "a"); need(n >= 5, "pulses while toggled on (got %d)" % n, P)
        need(not keys_at(l, 900), "stopped after the second press", P)
    return p, [N(50), S(60, A=1), N(500), S(60, A=1), N(300)], o

@case("mode_double_release_conflict", "double press window: two quick presses give c, a slow second press gives a twice")
def _():
    p = Profile(); p.bind("A", key("a"), (), key("c"), mode=M_DOUBLE); p.times(dbl=200)
    def o(l, P): need(taps(l, "c") == 1 and taps(l, "a") == 2, "c once, a twice (a %d c %d)" % (taps(l, "a"), taps(l, "c")), P)
    return p, [N(50), S(50, A=1), N(80), S(50, A=1), N(400), S(50, A=1), N(400), S(50, A=1), N(300)], o

@case("chord_turbo", "B turbo, chorded with LB")
def _():
    p = Profile(); p.bind("B", key("b"), mode=M_TURBO, chord="LB"); p.times(turbo=80)
    def o(l, P): need(taps(l, "b") >= 4 and first(l, "b") >= 250, "b pulses only with LB (got %d)" % taps(l, "b"), P)
    return p, [N(50), S(200, B=1), S(50, LB=1), S(500, LB=1, B=1), N(200)], o

@case("macro_long_mods", "macro of 8 steps with shift held over two letters")
def _():
    p = Profile(); p.macro(1, [(key("lshift"), 0), (key("h"), 3), (0, 3), (key("i"), 3), (0, 3), (key("enter"), 3)])
    p.bind("Y", MACRO + 1)
    def o(l, P):
        need(ever(l, "h") and ever(l, "i") and ever(l, 0x28), "h, i, enter", P)
    return p, [N(50), S(40, Y=1), N(600)], o

@case("layer_nested", "a layer's own layer action is ignored (no nesting, by design): A stays x")
def _():
    p = Profile(); p.bind("A", key("a")); p.bind("L4", LAYER + LSLOT)
    lay = Profile("lay1"); lay.bind("A", key("x")); lay.bind("R4", LAYER + 1); _extra[LSLOT] = lay
    lay2 = Profile("lay2"); lay2.bind("A", key("y")); _extra[1] = lay2
    def o(l, P): need(taps(l, "a") == 1 and taps(l, "x") == 2 and taps(l, "y") == 0, "a, x, x (a %d x %d y %d)" % (taps(l, "a"), taps(l, "x"), taps(l, "y")), P)
    return p, [N(50)] + tapb("A") + [S(80, L4=1), S(100, L4=1, A=1), S(100, L4=1), S(80, L4=1, R4=1), S(100, L4=1, R4=1, A=1), S(100, L4=1, R4=1), N(150)], o

@case("layer_by_ring", "the RS outer ring holds a layer where A = x")
def _():
    p = Profile(); p.analog(1, DPAD4, ring=5000); p.bind("RS_RING", LAYER + LSLOT); p.bind("A", key("a"))
    lay = Profile("ring"); lay.analog(1, DPAD4, ring=5000); lay.bind("A", key("x")); _extra[LSLOT] = lay
    def o(l, P): need(taps(l, "x") == 1 and taps(l, "a") == 1, "x with the ring, a without", P)
    return p, [N(50), S(100, rsx=32000), S(120, rsx=32000, A=1), S(100, rsx=32000), N(100), S(120, A=1), N(150)], o

@case("profile_switch_block", "switching into a profile with unbound inputs blocked: the switch button does not act there")
def _():
    p = Profile(); p.bind("L5", PROFILE + LSLOT)
    q = Profile("blk"); q.flags(PF_BLOCK); q.bind("L5", key("z")); q.bind("A", PROFILE + SLOT); _extra[LSLOT] = q
    def o(l, P):
        need(not ever(l, "z"), "the held L5 does not fire z in the new profile", P)
        need(l[-1]["prof"] == SLOT and any(e["prof"] == LSLOT for e in l), "went over and back", P)
    return p, [N(50), S(200, L5=1), N(150), S(80, A=1), N(150)], o

@case("stick_mouse_curve_haptic", "LS mouse with curve 2.0 and a movement tick")
def _():
    p = Profile(); p.analog(0, MOUSE, speed=2000, curve=200, haptic=1)
    def o(l, P):
        half = at(l, 50 + 400); need(half is not None and 80 < half["dx"] < 400, "half deflection squared: ~250 in 0.4 s (got %s)" % (half and half["dx"]), P)
        need(l[-1]["hap"] >= 1, "ticks while moving", P)
    return p, [N(50), S(400, lsx=16384), S(300, lsx=32767), N(150)], o

@case("stick_flick_circle", "RS flick: one full circle of the stick = one turn")
def _():
    p = Profile(); p.analog(1, FLICK, speed=100)
    st = [N(50), S(150, rsy=32767)] + [stick(1, 90 - d, 32767, 20) for d in range(0, 361, 15)][:28] + [N(200)]
    def o(l, P): need(abs(l[-1]["dx"]) > 10000, "turned most of a circle (dx %d)" % l[-1]["dx"], P)
    return p, st, o

@case("stick_radial_4", "RS radial 4 slices")
def _():
    p = Profile(); p.analog(1, RADIAL, dz_in=3000); p.anx(1, slices=4)
    for i, k in enumerate("1234"): p.bind((34, 37, 35, 36)[i], key(k))
    def o(l, P): need(taps(l, "2") == 1 and taps(l, "4") == 1, "right then left (2 %d 4 %d)" % (taps(l, "2"), taps(l, "4")), P)
    return p, [N(50), S(120, rsx=30000), N(150), S(120, rsx=-30000), N(150)], o

@case("stick_dpad_rotate_invert", "LS 4-way rotated 90 and RS 4-way inverted x")
def _():
    p = Profile(); p.analog(0, DPAD4, dz_in=3000, rotate=90); p.analog(1, DPAD4, dz_in=3000, flags=AF_INVX)
    p.bind("LS_UP", key("w")); p.bind("LS_RIGHT", key("d")); p.bind("LS_LEFT", key("a")); p.bind("RS_LEFT", key("j")); p.bind("RS_RIGHT", key("l"))
    def o(l, P):
        need(ever(l, "a") or ever(l, "d"), "rotated: up becomes a side", P)
        need(ever(l, "j") and not ever(l, "l"), "inverted x: right becomes left", P)
    return p, [N(50), S(150, lsy=30000), N(), S(150, rsx=30000), N(150)], o

@case("stick_ring_pass", "LS to Steam with an outer ring key")
def _():
    p = Profile(); p.analog(0, PASS, ring=8000); p.bind("LS_RING", key("lshift"))
    def o(l, P): need(taps(l, "lshift") == 1 and first(l, "lshift") >= 200, "shift only past the ring", P)
    return p, [N(50), S(150, lsx=20000), S(150, lsx=31000), N(150)], o

@case("pad_radial_click", "LP radial firing on click, 8 slices")
def _():
    p = Profile(); p.analog(2, RADIAL, speed=8, dz_in=2500, flags=AF_CLICK)
    for i, k in enumerate("12345678"): p.bind((38, 47, 41, 49, 39, 48, 40, 46)[i], key(k))
    def o(l, P): need(sum(taps(l, k) for k in "12345678") == 1, "one slice fired (on the click)", P)
    return p, [N(50), pad(0, 0, 0, 60), pad(0, 25000, 0, 120), pad(0, 25000, 0, 100, True), pad(0, 25000, 0, 60), N(200)], o

@case("pad_swipe_repeat", "RP long swipe with repeat: fires more than once")
def _():
    p = Profile(); p.analog(3, SWIPE, speed=3000, flags=AF_SWREP); p.bind("RP_RIGHT", key("d"))
    def o(l, P): need(taps(l, "d") >= 2, "a long swipe repeats (got %d)" % taps(l, "d"), P)
    return p, [N(50)] + [pad(1, -30000 + i * 3000, 0, 10) for i in range(21)] + [N(200)], o

@case("pad_edge_spin", "RP mouse: finger resting at the edge keeps turning")
def _():
    p = Profile(); p.analog(3, MOUSE, speed=100); p.anx(3, edge_spin=2000)
    def o(l, P): need(l[-1]["dx"] > 200, "edge spin moves on (dx %d)" % l[-1]["dx"], P)
    return p, [N(50)] + [pad(1, 20000 + i * 2000, 0, 10) for i in range(6)] + [pad(1, 32000, 0, 500), N(150)], o

@case("pad_joystick_same", "LP joystick to the left stick", snap=True)
def _():
    p = Profile(); p.analog(2, JOYSTICK, dz_in=500)
    def o(l, P): need(l[1]["lsy"] > 10000, "finger up -> left stick up (lsy %d)" % l[1]["lsy"], P)
    return p, [N(50), pad(0, 0, 16000, 80), N(80)], o

@case("pad_mouse_joystick", "RP mouse joystick: the finger's speed is the right stick, resting = centred (v51 test 62)", snap=True)
def _():
    p = Profile(); p.analog(3, MOUSEJOY, speed=100)
    st = [N(50), pad(1, 20000, 0, 40)] + [pad(1, 20000 - i * 1200, 0, 5) for i in range(1, 21)] + [pad(1, -4000, 0, 80), N(80)]
    def o(l, P):
        need(abs(l[1]["rsx"]) < 3000, "finger resting: centred (rsx %d)" % l[1]["rsx"], P)
        need(min(e["rsx"] for e in l[5:22]) < -10000, "finger moving left: stick left (min rsx %d)" % min(e["rsx"] for e in l[5:22]), P)
        need(abs(l[-2]["rsx"]) < 3000, "finger stopped: centred again (rsx %d)" % l[-2]["rsx"], P)
    return p, st, o

@case("pad_xstick", "RP to the Xbox right stick (Steam pad stick without the Xbox output)", snap=True)
def _():
    p = Profile(); p.analog(3, XSTICK)
    def o(l, P): need(l[1]["rsx"] > 10000 or l[1]["xs"][2] > 10000, "finger right reaches the stick", P)
    return p, [N(50), pad(1, 20000, 0, 80), N(80)], o

@case("pad_gates_click", "RP gates needing a click")
def _():
    p = Profile(); p.analog(3, GATES, dz_in=2000, flags=AF_CLICK)
    for i, k in enumerate("qepz"): p.bind(50 + i, key(k))
    def o(l, P): need(taps(l, "q") == 1 and first(l, "q") >= 170, "q only on the click", P)
    return p, [N(50), pad(1, -20000, 20000, 120), pad(1, -20000, 20000, 120, True), N(150)], o

@case("shift_lp_mode", "R5 shifts LP from 4-way to mouse; RP click shifted with L5")
def _():
    p = Profile(); p.analog(2, DPAD4, dz_in=3000); p.bind("LP_RIGHT", key("d")); p.shift(4, BTN["R5"], mode=MOUSE, speed=100)
    def o(l, P): need(taps(l, "d") == 1, "unshifted d", P); need(l[-1]["dx"] > 10, "shifted: mouse (dx %d)" % l[-1]["dx"], P)
    return p, ([N(50), pad(0, 25000, 0, 150), N(), S(60, R5=1)] + [S(8, R5=1, LP_TOUCH=1, lpx=-15000 + i * 1500) for i in range(18)] + [N(150)]), o

@case("shift_dpad_4way", "L4 shifts the D-pad to 4-way with other keys")
def _():
    p = Profile(); p.bind("DUP", key("w")); p.bind("DRIGHT", key("d")); p.shift(1, BTN["L4"], dpad4=1)
    p.bind("DUP", key("i"), shifted_index=SHIFT_SRC.index(13)); p.bind("DRIGHT", key("l"), shifted_index=SHIFT_SRC.index(11))
    def o(l, P):
        need(any({key("w"), key("d")} <= set(e["keys"]) for e in l), "unshifted 8-way: w+d", P)
        need(not any({key("i"), key("l")} <= set(e["keys"]) for e in l) and (ever(l, "i") or ever(l, "l")), "shifted 4-way: i or l, never both", P)
    return p, [N(50), S(150, DUP=1, DRIGHT=1), N(), S(60, L4=1), S(150, L4=1, DUP=1, DRIGHT=1), N(150)], o

@case("shift_trigger_soft", "LT shift group: soft pull s -> 1 while L5 held")
def _():
    p = Profile(); p.trig_soft(0, 30); p.bind("LT_SOFT", key("s")); p.shift(6, BTN["L5"]); p.bind("LT_SOFT", key("1"), shifted_index=SHIFT_SRC.index(58))
    def o(l, P): need(taps(l, "s") == 1 and taps(l, "1") == 1, "s, then 1 shifted", P)
    return p, [N(50), S(120, tl=16000), N(), S(60, L5=1), S(120, L5=1, tl=16000), N(150)], o

@case("trig_range_left_curve_right", "LT out range 10..50 %, RT curve 0.5", snap=True)
def _():
    p = Profile(); struct.pack_into("<HH", p.b, 110, 1000, 5000); p.trig_curve(1, 50)
    def o(l, P):
        need(l[1]["tl"] == 0 and l[3]["tl"] == 32767, "LT 10..50 %% (%d, %d)" % (l[1]["tl"], l[3]["tl"]), P)
        need(14500 < l[5]["tr"] < 18500, "RT quarter pull with curve 0.5 ~ half (got %d)" % l[5]["tr"], P)
    return p, [N(50), S(60, tl=2000), S(60, tl=9800), S(60, tl=20000), N(60), S(60, tr=8192), N(60)], o

# ===== every setting of the page: sweeps over each slider's range =====
def _sweep(name, what, vals, mk, **kw):
    for v in vals:
        def f(v=v): return mk(v)
        tag = "_".join(str(x) for x in v) if isinstance(v, tuple) else str(v)
        case("%s_%s" % (name, tag.replace(".", "p").replace("-", "m")), what % (v,), **kw)(f)

def _stick_mouse_speed(v):
    p = Profile(); p.analog(0, MOUSE, speed=v)
    def o(l, P):
        exp = v * 0.4; got = l[-1]["dx"]
        need(abs(got - exp) <= max(3, 0.12 * exp), "0.4 s full right at %d/s: ~%d (got %d)" % (v, exp, got), P)
    return p, [N(50), S(400, lsx=32767), N(100)], o
_sweep("set_stick_mouse_speed", "LS mouse sensitivity %s counts/s", (50, 1000, 10000), _stick_mouse_speed)

def _stick_mouse_curve(v):
    p = Profile(); p.analog(0, MOUSE, speed=4000, curve=int(v * 100), dz_in=300)
    def o(l, P):
        m = (16384 / 32767 - 0.03) / 0.97
        exp = 4000 * 0.5 * m ** v; got = l[-1]["dx"]
        need(abs(got - exp) <= max(4, 0.15 * exp), "half deflection, curve %s: ~%d (got %d)" % (v, exp, got), P)
    return p, [N(50), S(500, lsx=16384), N(100)], o
_sweep("set_stick_mouse_curve", "LS mouse response curve %s", (0.5, 1, 2, 4), _stick_mouse_curve)

def _dz_in(v):
    p = Profile(); p.analog(0, PASS, dz_in=v * 100)
    def o(l, P):
        out = l[1]["lsx"]
        if v >= 30: need(out == 0, "30 %% push inside a %d %% dead zone: 0 (got %d)" % (v, out), P)
        else: need(out > 0, "30 %% push outside a %d %% dead zone: moves (got %d)" % (v, out), P)
        need(l[2]["lsx"] >= 32000, "full push: full (got %d)" % l[2]["lsx"], P)
    return p, [N(50), S(60, lsx=int(0.3 * 32767)), S(60, lsx=32767), N(60)], o
_sweep("set_stick_dz_in", "LS inner dead zone %s %%", (0, 25, 50, 90), _dz_in, snap=True)

def _dz_out(v):
    p = Profile(); p.analog(0, PASS, dz_out=v * 100)
    def o(l, P):
        out = l[1]["lsx"]
        if 60 >= 100 - v: need(out >= 32000, "60 %% push with a %d %% outer dead zone: full (got %d)" % (v, out), P)
        else: need(out < 32000, "60 %% push with a %d %% outer dead zone: not full (got %d)" % (v, out), P)
    return p, [N(50), S(60, lsx=int(0.6 * 32767)), N(60)], o
_sweep("set_stick_dz_out", "LS outer dead zone %s %%", (0, 25, 50), _dz_out, snap=True)

def _anti(v):
    p = Profile(); p.analog(0, PASS, dz_in=1000, anti=v * 100)
    def o(l, P):
        out = l[1]["lsx"] / 32767
        need(out >= v / 100 - 0.03, "just past the dead zone: at least the anti dead zone %d %% (got %.0f %%)" % (v, out * 100), P)
        need(out <= v / 100 + 0.1, "not much more (got %.0f %%)" % (out * 100), P)
    return p, [N(50), S(60, lsx=int(0.11 * 32767)), N(60)], o
_sweep("set_stick_anti", "LS anti dead zone %s %%", (0, 45, 90), _anti, snap=True)

def _rot(v):
    p = Profile(); p.analog(0, PASS, rotate=v)
    def o(l, P):
        a = math.atan2(l[1]["lsy"], l[1]["lsx"]) * 180 / math.pi
        exp = 90 + v  # positive = counter-clockwise
        d = (a - exp + 540) % 360 - 180
        need(abs(d) < 6, "up rotated by %d: at %.0f deg (want %d)" % (v, a, exp), P)
    return p, [N(50), S(60, lsy=25000), N(60)], o
_sweep("set_stick_rotate", "LS rotation %s degrees", (-90, -45, 45, 90), _rot, snap=True)

def _scale(v):
    p = Profile(); p.analog(1, MOUSE, speed=1000); p.anx(1, scale_x=v, scale_y=100)
    def o(l, P):
        exp = 1000 * 0.4 * v / 100; got = l[-1]["dx"]
        need(abs(got - exp) <= max(3, 0.12 * exp), "horizontal %d %%: ~%d (got %d)" % (v, exp, got), P)
    return p, [N(50), S(400, rsx=32767), N(100)], o
_sweep("set_stick_scale_x", "RS mouse horizontal scale %s %%", (10, 100, 400), _scale)

def _ring(v):
    p = Profile(); p.analog(0, PASS, ring=v * 100); p.bind("LS_RING", key("r"))
    pts = (0.3, 0.6, 0.95)
    def o(l, P):
        for i, r in enumerate(pts):
            on = key("r") in l[1 + i]["keys"]; want = r * 100 >= v
            need(on == want, "push %d %% with the ring at %d %%: %s (got %s)" % (r * 100, v, want, on), P)
    return p, [N(50)] + [S(80, lsx=int(r * 32767)) for r in pts] + [N(60)], o
_sweep("set_stick_ring", "LS outer ring at %s %%", (20, 50, 90), _ring, snap=True)

def _single(v):
    p = Profile(); p.analog(0, SINGLE, dz_in=v * 100); p.bind("LS_RING", key("x"))
    pts = (0.2, 0.5, 0.97)
    def o(l, P):
        for i, r in enumerate(pts):
            on = key("x") in l[1 + i]["keys"]; want = r * 100 >= max(v, 5)
            need(on == want, "whole stick pushed %d %%, threshold %d %%: %s (got %s)" % (r * 100, v, want, on), P)
    return p, [N(50)] + [S(80, lsx=int(r * 32767)) for r in pts] + [N(60)], o
_sweep("set_stick_single", "LS as one button, pushed past %s %%", (10, 40, 95), _single, snap=True)

def _soft(v):
    p = Profile(); p.trig_soft(0, v); p.bind("LT_SOFT", key("s"))
    pts = (0.02, 0.4, 0.75, 1.0)
    def o(l, P):
        for i, r in enumerate(pts):
            on = key("s") in l[1 + i]["keys"]; want = r * 100 >= v
            need(on == want, "pull %d %%, soft point %d %%: %s (got %s)" % (r * 100, v, want, on), P)
    return p, [N(50)] + [S(80, tl=int(r * 32767)) for r in pts] + [N(60)], o
_sweep("set_trig_soft", "LT soft pull point %s %%", (1, 50, 100), _soft, snap=True)

def _trange(v):
    a, e = v
    p = Profile(); struct.pack_into("<HH", p.b, 110, a * 100, e * 100)
    pts = (a / 100 * 0.5, (a + e) / 200, min(1.0, e / 100 + 0.02))
    def o(l, P):
        need(l[1]["tl"] == 0, "below the start: 0 (got %d)" % l[1]["tl"], P)
        need(12000 < l[2]["tl"] < 21000, "half way: about half (got %d)" % l[2]["tl"], P)
        need(l[3]["tl"] >= 32000, "past the end: full (got %d)" % l[3]["tl"], P)
    return p, [N(50)] + [S(80, tl=int(r * 32767)) for r in pts] + [N(60)], o
_sweep("set_trig_range", "LT analog range %s", ((10, 60), (40, 100), (0, 30)), _trange, snap=True)

def _hipwin(v):
    p = Profile(); p.trig_soft(1, 30); p.trig_mode(1, 2, hip_ms=v); p.bind("RT_SOFT", key("s")); p.bind("RT_CLICK", key("f"))
    pull = 60  # ms from the soft point to the click
    def o(l, P):
        if pull < v: need(taps(l, "s") == 0 and taps(l, "f") == 1, "full within the %d ms window: only f (s %d f %d)" % (v, taps(l, "s"), taps(l, "f")), P)
        else: need(taps(l, "s") == 1 and taps(l, "f") == 1, "slower than the %d ms window: s then f (s %d f %d)" % (v, taps(l, "s"), taps(l, "f")), P)
    return p, [N(50), S(pull, tr=14000), S(250, tr=32767, RT_CLICK=1), N(200)], o
_sweep("set_trig_hip_window", "RT hip fire window %s ms", (20, 75, 400), _hipwin)

def _pturbo(v):
    p = Profile(); p.bind("A", key("a"), mode=M_TURBO); p.times(turbo=v)
    hold = min(1000, 15 * v)
    def o(l, P):
        exp = hold / v; n = taps(l, "a")
        need(abs(n - exp) <= max(1, 0.15 * exp), "%d ms turbo held %d ms: ~%.0f pulses (got %d)" % (v, hold, exp, n), P)
    return p, [N(50), S(hold, A=1), N(300)], o
_sweep("set_profile_turbo", "profile turbo period %s ms", (40, 100, 500), _pturbo, tol_ms=15)

def _phold(v):
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_HOLD); p.times(hold=v)
    def o(l, P):
        need(taps(l, "a") == 1 and taps(l, "b") == 1, "short press a, press longer than %d ms b (a %d b %d)" % (v, taps(l, "a"), taps(l, "b")), P)
    return p, [N(50), S(v - 60, A=1), N(300), S(v + 80, A=1), N(300)], o
_sweep("set_profile_hold", "profile long press time %s ms", (100, 250, 1000), _phold)

def _pdbl(v):
    p = Profile(); p.bind("A", key("a"), (), key("c"), mode=M_DOUBLE); p.times(dbl=v)
    def o(l, P):
        need(taps(l, "c") == 1, "second press inside %d ms: c once (got %d)" % (v, taps(l, "c")), P)
        need(taps(l, "a") == 2, "two presses further apart than %d ms: a twice (got %d)" % (v, taps(l, "a")), P)
    return p, [N(50), S(40, A=1), S(v - 80), S(40, A=1), N(v + 300), S(40, A=1), S(v + 60), S(40, A=1), N(v + 300)], o
_sweep("set_profile_double", "profile double press window %s ms", (100, 250, 600), _pdbl)

def _bt(v):
    hold, turbo, ds, de = v
    p = Profile(); p.bind("A", key("a"), key("b"), mode=M_HOLD | M_TURBO); p.btime("A", hold=hold, turbo=turbo)
    p.bind("B", key("x")); p.btime("B", dstart=ds, dend=de)
    def o(l, P):
        need(taps(l, "a") >= 1, "a short press: a", P)
        nb = taps(l, "b"); exp = 600 / turbo
        need(abs(nb - exp) <= max(1, 0.2 * exp), "held %d ms past the %d ms long press: ~%.0f b pulses every %d ms (got %d)" % (600, hold, exp, turbo, nb), P)
        tx = first(l, "x"); t0 = 50 + 60 + 300 + hold + 600 + 300
        need(tx is not None and abs(tx - (t0 + ds)) <= 15, "x %d ms after B (at %s, B at %d)" % (ds, tx, t0), P)
        off = next((e["t"] for e in l if tx and e["t"] > tx and key("x") not in e["keys"]), None)
        need(off is not None and abs(off - (t0 + 150 + ds + de)) <= 15, "x off %d ms after B is let go, delayed by %d (off at %s, let go at %d)" % (de, ds, off, t0 + 150), P)
    return p, [N(50), S(60, A=1), N(300), S(hold + 600, A=1), N(300), S(150, B=1), N(ds + de + 300)], o
_sweep("set_binding_times", "binding times hold/turbo/delay start/end %s", ((100, 50, 0, 200), (500, 100, 300, 0), (1000, 200, 1000, 1000)), _bt, tol_ms=15)

def _mvspeed(v):
    p = Profile(); p.bind("DRIGHT", MOVE_RIGHT); p.move_speed(v)
    def o(l, P):
        exp = v * 0.5; got = l[-1]["dx"]
        need(abs(got - exp) <= max(3, 0.1 * exp), "move right 0.5 s at %d/s: ~%d (got %d)" % (v, exp, got), P)
    return p, [N(50), S(500, DRIGHT=1), N(100)], o
_sweep("set_move_speed", "mouse move action speed %s/s", (50, 1000, 10000), _mvspeed)

def _scroll_speed(v):
    p = Profile(); p.analog(1, SCROLL, speed=int(v * 100))
    def o(l, P):
        exp = v * 1.0; got = l[-1]["wh"]
        need(abs(got - exp) <= max(1.5, 0.15 * exp), "1 s full up at %s notches/s: ~%.0f (got %d)" % (v, exp, got), P)
    return p, [N(50), S(1000, rsy=32767), N(100)], o
_sweep("set_stick_scroll_speed", "RS scroll speed %s notches/s", (1, 10, 50), _scroll_speed)

def _wheel_step(v):
    p = Profile(); p.analog(2, SCROLL, speed=v); p.bind("LP_CW", key("q"))
    turn = min(330, 10 * v)
    def o(l, P):
        exp = turn / v; n = taps(l, "q")
        need(abs(n - exp) <= max(2, 0.25 * exp), "%d degrees with a %d degree step: ~%.0f (got %d)" % (turn, v, exp, n), P)
    return p, [N(50)] + ring_points(0, 25000, 90, 90 - turn, 22, 20) + [N(1500)], o
_sweep("set_pad_wheel_step", "LP scroll wheel step %s degrees", (15, 45, 90), _wheel_step, tol_ms=60)

def _pad_sens(v):
    p = Profile(); p.analog(3, MOUSE, speed=v)
    def o(l, P):
        exp = v * 24000 / 65535; got = l[-1]["dx"]
        need(abs(got - exp) <= max(3, 0.12 * exp), "a slide over 24000 pad units at %d per pad: ~%.0f (got %d)" % (v, exp, got), P)
    return p, [N(50)] + [pad(1, -12000 + i * 1000, 0, 8) for i in range(25)] + [N(150)], o
_sweep("set_pad_mouse_sens", "RP mouse sensitivity %s", (50, 2000, 20000), _pad_sens)

def _pad_accel(v):
    p = Profile(); p.analog(3, MOUSE, speed=2000, curve=v)
    def o(l, P):
        base = 2000 * 24000 / 65535; got = l[-1]["dx"]
        need(got >= base * 0.88 and (v == 0 or got > base * 1.05) and got <= base * (1 + v / 100) * 1.12, "acceleration %d %%: %.0f..%.0f (got %d)" % (v, base, base * (1 + v / 100), got), P)
    return p, [N(50)] + [pad(1, -12000 + i * 2000, 0, 4) for i in range(13)] + [N(150)], o
_sweep("set_pad_mouse_accel", "RP mouse acceleration %s %%", (0, 100, 300), _pad_accel)

def _friction(v):
    p = Profile(); p.analog(3, MOUSE, flags=AF_TRACKBALL, speed=2000, dz_out=int(v * 100))
    st = [N(50)] + [pad(1, -15000 + i * 1500, 0, 5) for i in range(20)] + [N(500) for _ in range(6)]
    def o(l, P):
        v0 = 1500 / 0.005 * 2000 / 65535  # counts per second at the lift
        exp = v0 / v * (1 - math.exp(-v * 3.0))
        roll = l[-1]["dx"] - l[20]["dx"]
        need(abs(roll - exp) <= 0.3 * exp + 20, "friction %s: rolls ~%.0f after lifting (got %d)" % (v, exp, roll), P)
    return p, st, o
_sweep("set_pad_friction", "RP trackball friction %s", (0.5, 5, 20), _friction, snap=True, rel=0.25)

def _edge(v):
    p = Profile(); p.analog(3, MOUSE, speed=100); p.anx(3, edge_spin=v)
    def o(l, P):
        got = l[-1]["dx"]
        if v == 0: need(got < 60, "no edge spin: stops (dx %d)" % got, P)
        else: need(got > v * 0.3, "edge spin %d/s for 0.6 s: keeps going (dx %d)" % (v, got), P)
    return p, [N(50)] + [pad(1, 20000 + i * 2000, 0, 10) for i in range(6)] + [pad(1, 32000, 0, 600), N(150)], o
_sweep("set_pad_edge_spin", "RP mouse edge spin %s", (0, 1000, 5000), _edge)

def _swipe_len(v):
    p = Profile(); p.analog(3, SWIPE, dz_in=v * 100); p.bind("RP_RIGHT", key("d"))
    def o(l, P):
        # a slide over 60 % of the pad's width (30 % of the full -1..1 range)
        fired = taps(l, "d") > 0
        need(fired == (v <= 55), "a 60 %% swipe with the length at %d %%: %s (got %s)" % (v, v <= 55, fired), P)
    return p, [N(50)] + [pad(1, -9800 + i * 1500, 0, 8) for i in range(14)] + [N(200)], o
_sweep("set_pad_swipe_length", "RP swipe length %s %%", (10, 40, 90), _swipe_len)

def _slices(v):
    p = Profile(); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=v)
    srcs = (30, 70, 33, 71, 31, 72, 32, 73)
    for i in range(v): p.bind(srcs[i], key(str(i + 1)))
    def o(l, P):
        fired = [k for k in range(1, v + 1) if taps(l, str(k))]
        need(fired == [1], "%d slices, pointed up: slice 1 (got %s)" % (v, fired), P)
    return p, [N(50), S(150, lsy=28000), N(200)], o
_sweep("set_stick_radial_slices", "LS radial with %s slices", (2, 3, 5, 8), _slices)

def _flick(v):
    p = Profile(); p.analog(1, FLICK, speed=v, dz_in=8000)
    def o(l, P):
        r = at(l, 50 + min(v, 300) // 2)
        need(abs(l[-1]["dx"] - 4091) < 300, "quarter turn in the end (dx %d)" % l[-1]["dx"], P)
        if v >= 300: need(r is not None and r["dx"] < 3500, "the %d ms flick is not done half way (dx %s)" % (v, r and r["dx"]), P)
    return p, [N(50), S(600, rsx=32767), N(300)], o
_sweep("set_flick_time", "RS flick time %s ms", (0, 100, 500), _flick)

def _press(v):
    p = Profile(); p.analog(2, DPAD4, flags=AF_PRESS, dz_in=3000, press=v * 100); p.bind("LP_UP", key("w"))
    pts = (0.05, 0.5, 1.0)
    def o(l, P):
        for i, r in enumerate(pts):
            on = key("w") in l[1 + i]["keys"]; want = r * 100 >= v
            need(on == want, "pressure %d %%, needed %d %%: %s (got %s)" % (r * 100, v, want, on), P)
    return p, [N(50)] + [pad(0, 0, 25000, 80, p=int(r * 32767)) for r in pts] + [N(60)], o
_sweep("set_pad_pressure", "LP pressure needed %s %%", (1, 30, 100), _press, snap=True)

def _tilt_dz(v):
    q = Profile("tiltdz"); q.gyx(mode=1, tilt_dz=v); q.bind("GYRO_UP", key("w"))
    st = _gyro_case(q, [(v - 3 if v > 5 else 1, 0, 300), (0, 0, 200), (v + 5, 0, 300), (0, 0, 200)])
    def o(l, P): need(taps(l, "w") == 1 and first(l, "w") >= 330 + 500, "tilted less than %d deg: nothing; more: w (w at %s)" % (v, first(l, "w")), P)
    return _gp(st, o)
_sweep("set_gyro_tilt_needed", "gyro tilt needed %s degrees", (5, 15, 45), _tilt_dz)

def _region_deg(v):
    q = Profile("regdeg"); q.gyx(mode=2, region_deg=v)
    st = _gyro_case(q, []) + [S(500, az=14000, gz=-int(v / 2 / 0.5 / 0.06103)), S(200, az=14000)]
    def o(l, P):
        xs = [e["absx"] for e in l if e["absx"] != 0xFFFF]
        need(xs and max(xs) > 31000, "turning half the area width (%d deg) reaches the right edge (max %s)" % (v // 2, max(xs, default=None)), P)
    return _gp(st, o)
_sweep("set_gyro_region_deg", "gyro region %s degrees across", (20, 60, 180), _region_deg)


# real Steam Input layouts (hwt_vdf.py)
sys.modules.setdefault("hwt", sys.modules[__name__])
import hwt_vdf
hwt_vdf.register()


def build():
    out = []
    for c in CASES:
        _extra.clear()
        p, steps, _ = c["f"]()
        out.append(dict(name=c["name"], what=c["what"], profile=bytes(p.b).hex(), script=script_bytes(steps).hex(),
                        ms=sum(ms for ms, _ in steps), snap=c["snap"], tol=c["tol"], rel=c["rel"], slot=c["slot"], blip=c["blip"], extra={str(k): bytes(v.b).hex() for k, v in _extra.items()}))
    return out


def oracle(name, log):
    c = next(c for c in CASES if c["name"] == name)
    _extra.clear(); _, _, o = c["f"](); P = []
    if not log: return ["empty log"]
    try: o(log, P)
    except Exception as e: P.append("oracle error %r" % e)
    return P


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "self"
    if cmd == "export":
        cases = build()
        for c in cases: c["sim"] = sim_run(c)["log"]
        doc = dict(version=2, slot=SLOT, cfg=cfg_names(), cases=cases)
        path = os.path.join(HERE, "../../web/hwtests.json")
        json.dump(doc, open(path, "w"), separators=(",", ":"))
        print("wrote %s: %d cases" % (path, len(cases)))
    elif cmd == "self":
        bad = 0
        only = set(sys.argv[2:])
        for c in build():
            if only and c["name"] not in only: continue
            r = sim_run(c)
            P = oracle(c["name"], r["log"])
            if r["flags"] & 8: P.insert(0, "log full")
            bad += bool(P)
            print(("ok  " if not P else "BAD ") + "%-30s %3d entries" % (c["name"], len(r["log"])))
            for x in P: print("    " + x)
        print("%d bad" % bad)
        sys.exit(1 if bad else 0)
    elif cmd == "check":
        res = json.load(open(sys.argv[2]))
        cases = {c["name"]: c for c in build()}
        snaps = {c["name"]: c["snap"] for c in CASES}
        tols = {c["name"]: c["tol"] for c in CASES}
        nbad = 0
        for r in res["results"]:
            c = cases.get(r["name"])
            if c is None: print("?   %s (not in this version)" % r["name"]); continue
            if r.get("error"): print("ERR %-30s %s" % (r["name"], r["error"])); nbad += 1; continue
            sim = sim_run(c, res.get("cfg"))["log"]
            hw = parse_log(bytes.fromhex(r["log"]))
            rels = {c["name"]: c["rel"] for c in CASES}
            probs = ["controller vs simulator: " + x for x in same(hw, sim, tols[r["name"]], snaps[r["name"]], rels[r["name"]], c["blip"], c["ms"])]
            probs += ["on the controller: " + x for x in oracle(r["name"], hw)]
            if r.get("flags", 0) & 16: probs.insert(0, "stopped early (Steam button or an upload)")
            if r.get("flags", 0) & 8: probs.insert(0, "log full")
            nbad += bool(probs)
            print(("ok  " if not probs else "BAD ") + r["name"])
            for pr in probs: print("    " + pr)
        print("%d of %d differ" % (nbad, len(res["results"])))


if __name__ == "__main__":
    main()
