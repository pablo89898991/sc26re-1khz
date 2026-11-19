"""Exact properties of analog modes, triggers, shifts, layers and profile features."""
import random, sys, math, struct
from simlib import *

KEYS = [key(c) for c in "abcdefghijklmnopqrstuvwxyz"]
ERR = []
def err(*a): ERR.append(" ".join(str(x) for x in a))
def held(s):
    k = Events(s.log).kbd(); return set(k[-1]) if k else set()

GROUP = {0: ("lsx", "lsy", 30, None), 1: ("rsx", "rsy", 34, None), 2: ("lpx", "lpy", 38, "LP_TOUCH"), 3: ("rpx", "rpy", 42, "RP_TOUCH")}

def put(s, g, x, y, touch=True):
    kx, ky, _, t = GROUP[g]
    kw = {kx: int(x), ky: int(y)}
    if t: kw[t] = 1 if touch else 0
    return s.run(15, **kw)

def transform(an, x, y):
    if an.get("flags", 0) & AF_INVX: x = -x
    if an.get("flags", 0) & AF_INVY: y = -y
    a = math.radians(an.get("rotate", 0))
    return x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)

def p_dirs(r):
    g = r.randrange(4); mode = r.choice([DPAD4, DPAD8])
    an = dict(mode=mode, dz_in=r.choice([1000, 3000]), rotate=r.choice([0, 0, 30, -45, 90]), flags=r.choice([0, AF_INVX, AF_INVY, AF_INVX | AF_INVY]))
    p = Profile(); p.analog(g, **an)
    base = GROUP[g][2]
    for i in range(4): p.bind(base + i, KEYS[i])  # up down left right
    s = Sim(); s.use(p); s.run(5)
    for deg in range(0, 360, 7):
        m = r.choice([0.5, 0.9])
        x, y = 32767 * m * math.cos(math.radians(deg)), 32767 * m * math.sin(math.radians(deg))
        tx, ty = transform(an, x, y)
        a = math.degrees(math.atan2(ty, tx)) % 360
        if mode == DPAD8 and min(abs((a - 22.5) % 45 - 45), (a - 22.5) % 45) < 2: continue
        if mode == DPAD4 and min(abs((a - 45) % 90 - 90), (a - 45) % 90) < 2: continue
        put(s, g, x, y)
        if mode == DPAD4:
            want = {KEYS[0] if ty > 0 else KEYS[1]} if abs(ty) >= abs(tx) else {KEYS[2] if tx < 0 else KEYS[3]}
        else:
            sec = int((a + 22.5) // 45) % 8
            want = [{KEYS[3]}, {KEYS[3], KEYS[0]}, {KEYS[0]}, {KEYS[0], KEYS[2]}, {KEYS[2]}, {KEYS[2], KEYS[1]}, {KEYS[1]}, {KEYS[1], KEYS[3]}][sec]
        if held(s) != want:
            err("dirs g=%d an=%s deg=%d m=%.1f want %s got %s" % (g, an, deg, m, sorted(want), sorted(held(s)))); return
    put(s, g, 0, 0, touch=False)
    if held(s): err("dirs g=%d: keys left after centre %s" % (g, held(s)))

def p_gates_grid(r):
    j = r.randrange(2); g = 2 + j
    p = Profile()
    grid = r.random() < .5
    if grid:
        lines = [r.randrange(-6000, -1000), r.randrange(1000, 6000), r.randrange(1000, 6000), r.randrange(-6000, -1000)]
        struct.pack_into("<4h", p.b, 22 + 8 * j, *lines)
        p.analog(g, GRID)
    else:
        p.analog(g, GATES, dz_in=2000)
    base = GROUP[g][2]; gate = 46 + 4 * j; center = 64 + j
    srcs = {"up": base, "down": base + 1, "left": base + 2, "right": base + 3, "tl": gate, "tr": gate + 1, "bl": gate + 2, "br": gate + 3, "c": center}
    for i, (n, sidx) in enumerate(srcs.items()): p.bind(sidx, KEYS[i])
    kof = {n: KEYS[i] for i, n in enumerate(srcs)}
    s = Sim(); s.use(p); s.run(5)
    for _ in range(40):
        x, y = r.randrange(-32000, 32000), r.randrange(-32000, 32000)
        fx, fy = x / 32767, y / 32767
        if grid:
            l, rr, t, b = [v / 10000 for v in lines]
            if min(abs(fx - l), abs(fx - rr), abs(fy - t), abs(fy - b)) < .01: continue
            col = 0 if fx < l else (2 if fx > rr else 1); row = 0 if fy > t else (2 if fy < b else 1)
            want = {kof[["tl", "up", "tr", "left", "c", "right", "bl", "down", "br"][row * 3 + col]]}
        else:
            if abs(math.hypot(fx, fy) - .2) < .01 or abs(fx) < .002 or abs(fy) < .002: continue
            want = set() if math.hypot(fx, fy) < .2 else {kof[("tl" if fx < 0 else "tr") if fy >= 0 else ("bl" if fx < 0 else "br")]}
        put(s, g, x, y)
        if held(s) != want:
            err("%s pad %d at (%d,%d) want %s got %s" % ("grid" if grid else "gates", j, x, y, want, held(s))); return

def p_radial(r):
    j = r.randrange(2); n = r.randrange(2, 11)
    p = Profile(); p.analog(2 + j, RADIAL, speed=n, dz_in=3000)
    order = [[38, 47, 41, 49, 39, 48, 40, 46, 64, 54], [42, 51, 45, 53, 43, 52, 44, 50, 65, 56]][j]
    for k in range(n): p.bind(order[k], KEYS[k])
    s = Sim(); s.use(p); s.run(5)
    for _ in range(12):
        deg = r.uniform(0, 360)  # from up, clockwise
        w = 360 / n
        if min((deg + w / 2) % w, w - (deg + w / 2) % w) < 2: continue
        want = int((deg + w / 2) // w) % n
        x, y = 30000 * math.sin(math.radians(deg)), 30000 * math.cos(math.radians(deg))
        s.log = []
        put(s, 2 + j, 0, 0); put(s, 2 + j, x, y); put(s, 2 + j, 0, 0, touch=False); s.run(60)
        got = [k for k in range(n) if Events(s.log).taps(KEYS[k])]
        if got != [want]: err("radial n=%d deg=%.0f want slice %d got %s" % (n, deg, want, got)); return

def p_ring(r):
    g = r.randrange(4); R = r.choice([4000, 6000, 9000]); inv = r.random() < .5
    p = Profile(); p.analog(g, PASS if g < 2 else DPAD8, ring=R, dz_in=1000, flags=AF_RING_INV if inv else 0)
    p.bind(60 + g, KEYS[5])
    s = Sim(); s.use(p); s.run(5)
    for m in [0, .05, R / 10000 - .03, R / 10000 + .03, .98]:
        deg = r.uniform(0, 360)
        put(s, g, 32767 * m * math.cos(math.radians(deg)), 32767 * m * math.sin(math.radians(deg)), touch=m > 0)
        on = (m >= R / 10000) if not inv else (.1 <= m < R / 10000)
        if (KEYS[5] in held(s)) != on: err("ring g=%d R=%d inv=%s m=%.2f want %s" % (g, R, inv, m, on)); return

def p_hip(r):
    t = r.randrange(2); hip = r.choice([40, 75, 150]); mode = r.choice([2, 3])
    p = Profile(); p.trig_soft(t, 30); p.trig_mode(t, mode, hip)
    p.bind(58 + t, KEYS[0]); p.bind(27 if t == 0 else 23, KEYS[1])
    kt, kc = ("tl", "LT_CLICK") if t == 0 else ("tr", "RT_CLICK")
    for dur in [hip // 3, hip - 10, hip + 30, hip * 3]:
        s = Sim(); s.use(p); s.run(5); s.log = []
        # linear pull 0 -> 100 % over dur ms, the click at the end
        for ms in range(dur):
            v = int(32767 * (ms + 1) / dur)
            s.run(1, **{kt: v, kc: 1 if ms == dur - 1 else 0})
        s.run(100); s.run(100, **{kt: 0, kc: 0}); s.run(100)
        ev = Events(s.log)
        soft_at = 0.3 * dur  # when soft is crossed
        quick = (dur - soft_at) < hip
        a, b = ev.taps(KEYS[0]), ev.taps(KEYS[1])
        want = (0, 1) if quick else (1, 1)
        if (a, b) != want: err("hip t=%d mode=%d window=%d pull=%dms soft,full want %s got %s" % (t, mode, hip, dur, want, (a, b))); return

def p_speeds(r):
    sp = r.choice([500, 1000, 3000]); i = r.randrange(2)
    p = Profile(); p.analog(i, MOUSE, speed=sp)
    s = Sim(); s.use(p); s.run(5)
    e = s.run(1000, **{GROUP[i][0]: 32767})
    dx = sum(m[1] for m in e.mouse())
    if abs(dx - sp) > sp * .03 + 2: err("stick mouse speed %d: %d counts in 1 s" % (sp, dx))
    mv = r.choice([300, 1000, 2500])
    p = Profile(); p.move_speed(mv); p.bind("A", MOVE_DOWN)
    s = Sim(); s.use(p); s.run(5); e = s.run(1000, A=1)
    dy = sum(m[2] for m in e.mouse())
    if abs(dy - mv) > mv * .03 + 2: err("mouse move speed %d: %d counts" % (mv, dy))
    ns = r.choice([100, 500, 1000])
    p = Profile(); p.analog(i, SCROLL, speed=ns)
    s = Sim(); s.use(p); s.run(5); e = s.run(2000, **{GROUP[i][1]: 32767})
    w = sum(m[3] for m in e.mouse())
    if abs(w - 2 * ns / 100) > 1.01: err("stick scroll %d/100 notches/s: %d in 2 s" % (ns, w))

def p_wheel(r):
    j = r.randrange(2); step = r.choice([15, 30, 45, 90])
    p = Profile(); p.analog(2 + j, SCROLL, speed=step, dz_in=2000)
    p.bind(54 + 2 * j, KEYS[0]); p.bind(55 + 2 * j, KEYS[1])
    s = Sim(); s.use(p); s.run(5); s.log = []
    turns = r.choice([1, 2])
    for d in range(0, 360 * turns + 1, 3):
        a = math.radians(90 - d)
        s.run(2, **{GROUP[2 + j][0]: int(25000 * math.cos(a)), GROUP[2 + j][1]: int(25000 * math.sin(a)), GROUP[2 + j][3]: 1})
    s.run(600, **{GROUP[2 + j][3]: 0})
    cw = Events(s.log).taps(KEYS[0]); ccw = Events(s.log).taps(KEYS[1])
    exp = int(360 * turns / step)
    spin_ms = (360 * turns // 3) * 2
    cap = min(exp, 1 + max(0, spin_ms - 50) // 30 + 8)  # taps of 20 ms + 10 ms gaps, at most 8 waiting
    if ccw or not (cap - 3 <= cw <= exp):
        err("wheel step=%d turns=%d: cw %d ccw %d (expected %d..%d)" % (step, turns, cw, ccw, cap, exp))

def p_flick(r):
    i = r.randrange(2)
    p = Profile(); p.analog(i, FLICK, speed=r.choice([0, 50, 100]))
    s = Sim(); s.use(p); s.run(5)
    deg = r.uniform(-175, 175)
    e = s.run(400, **{GROUP[i][0]: int(32767 * math.sin(math.radians(deg))), GROUP[i][1]: int(32767 * math.cos(math.radians(deg)))})
    dx = sum(m[1] for m in e.mouse())
    exp = 16364 * deg / 360
    if abs(dx - exp) > 20: err("flick %.1f deg: %d counts, expected %.0f" % (deg, dx, exp))

def p_deadzones(r):
    i = r.randrange(2); dz = r.choice([0, 1000, 2500]); dzo = r.choice([0, 500, 2000]); anti = r.choice([0, 1500]); cv = r.choice([0, 100, 150, 200])
    p = Profile(); p.analog(i, PASS, dz_in=dz, dz_out=dzo, anti=anti, curve=cv)
    s = Sim(); s.use(p); s.run(5)
    prev = -1
    for v in range(0, 32768, 512):
        s.run(2, **{GROUP[i][0]: v, GROUP[i][1]: 0})
        o = s.out[GROUP[i][0]]
        if o < prev: err("dead zones not monotonic %s at %d: %d < %d" % ((dz, dzo, anti, cv), v, o, prev)); return
        if v / 32767 < dz / 10000 - .001 and o != 0: err("inside dead zone %d -> %d" % (v, o)); return
        if (dz or dzo or anti or cv not in (0, 100)) and v / 32767 > 1 - dzo / 10000 + .01 and o < 32700: err("outer dz %d -> %d" % (v, o)); return
        prev = o

def p_shift_layer(r):
    g = r.randrange(8); btn = r.choice(["L4", "L5", "R4", "R5"])
    srcs = SHIFT_SRC
    first = [0, 4, 8, 13, 18, 30, 42, 44, 46][g]; last = [0, 4, 8, 13, 18, 30, 42, 44, 46][g + 1]
    p = Profile(); p.shift(g, BTN[btn])
    k = r.randrange(first, last); src = srcs[k]
    if src >= 30: return  # analog sources: covered by the fuzzers
    p.bind(src, KEYS[0]); o = 1412 + 14 * k; p.b[o + 1] = KEYS[1]
    s = Sim(); s.use(p); s.run(5)
    name = [n for n, v in BTN.items() if v == src][0]
    s.run(20, **{name: 1}); a = held(s); s.run(20, **{name: 0})
    s.run(20, **{btn: 1}); s.run(20, **{name: 1}); b = held(s); s.run(20, **{name: 0}); s.run(20, **{btn: 0})
    if a != {KEYS[0]} or b != {KEYS[1]}: err("shift group %d (%s) source %s: normal %s shifted %s" % (g, btn, name, a, b))
    # layer
    base = Profile(); base.bind(src, KEYS[2]); base.bind(btn, LAYER + 1)
    lay = Profile(); lay.bind(src, KEYS[3])
    s = Sim(); s.upload(1, lay); s.use(base); s.run(5)
    s.run(20, **{btn: 1}); s.run(20, **{name: 1}); c = held(s); s.run(20, **{name: 0}); s.run(20, **{btn: 0})
    s.run(20, **{name: 1}); d = held(s); s.run(20, **{name: 0})
    if c != {KEYS[3]} or d != {KEYS[2]}: err("layer source %s: in layer %s after %s" % (name, c, d))

# the profile's gyro values in remap_gyro_keys order; the firmware holds each to its setting's range
# (v51 test 44: a profile made elsewhere can hold anything)
GKEYS = ["gm_output", "gm_act_button", "gm_act_mode", "gm_axis_pitch", "gm_axis_yaw", "gm_axis_roll", "gm_sens", "gm_scale_p",
         "gm_scale_y", "gm_scale_r", "gm_curve", "gm_accel", "gm_accel_lo", "gm_accel_hi", "gj_dz", "gj_anti", "gj_max", "gj_curve",
         "gj_tilt", "gm_haptic", "gm_qt1_btn", "gm_qt1_deg", "gm_qt2_btn", "gm_qt2_deg", "gm_qt3_btn", "gm_qt3_deg", "gm_qt4_btn",
         "gm_qt4_deg", "gm_qt_ms", "gm_ratchet"]

def p_profiles(r):
    s = Sim()
    rng = {n: struct.unpack("<hhhB", s.read("sc26re/" + n)[:7])[1:3] for n in GKEYS}
    profs = []
    for k in range(4):
        p = Profile("p%d" % k); p.bind("A", KEYS[k])
        if r.random() < .5: p.b[40:43] = bytes([r.randrange(1, 256), r.randrange(256), r.randrange(256)])
        p.flags(r.choice([0, PF_GYRO, PF_GYRO | PF_GYRO_ON]))
        for q in range(30): struct.pack_into("<h", p.b, 44 + 2 * q, r.randrange(-50, 500))
        profs.append(p); s.upload(k, p)
    s.cfg("remap_on", 1); s.run(5)
    led0 = bytes(3)
    for d, k in (("DUP", 0), ("DRIGHT", 1), ("DDOWN", 2), ("DLEFT", 3), ("DUP", 0)):
        s.run(20, STEAM=1, **{d: 1}); s.run(20, STEAM=0, **{d: 0}); s.run(20)
        if s.cfg_get("remap_profile") != k: err("Steam+%s -> profile %d (want %d)" % (d, s.cfg_get("remap_profile"), k)); return
        s.run(20, A=1); h = held(s); s.run(20, A=0)
        if h != {KEYS[k]}: err("profile %d: A gives %s" % (k, h))
        p = profs[k]
        for q in range(30):
            lo, hi = rng[GKEYS[q]]
            v = s.lib.sim_gyro_key(q); want = min(max(struct.unpack_from("<h", p.b, 44 + 2 * q)[0], lo), hi) if p.b[17] & PF_GYRO else -32768
            if v != want: err("profile %d gyro key %d: %d want %d" % (k, q, v, want)); break
        led = (ctypes.c_uint8 * 3)(); s.lib.sim_led(led)
        want = bytes(p.b[40:43]) if any(p.b[40:43]) else led0
        if bytes(led) != want: err("profile %d LED %s want %s" % (k, bytes(led).hex(), want.hex()))
        if s.lib.sim_gyro_mouse() != (1 if p.b[17] & PF_GYRO_ON else 0): err("profile %d gyro on=%d flags=%x" % (k, s.lib.sim_gyro_mouse(), p.b[17]))

def p_macro(r):
    steps = [(r.choice(KEYS[:6] + [0]), r.randrange(1, 40)) for _ in range(r.randrange(1, 8))]
    p = Profile(); p.macro(2, steps); p.bind("Y", MACRO + 2)
    s = Sim(); s.use(p); s.run(5); s.log = []
    t0 = s.lib.sim_time() + 1
    s.run(10, Y=1); s.run(3000, Y=0)
    # expected timeline: step k holds its key from start_k for t*10 ms
    t, exp = t0, []
    for a, d in steps:
        exp.append((a, t, t + d * 10)); t += d * 10
    def state_at(t):
        cur = set()
        for e in s.log:
            q = e.split()
            if q[1] != "K": continue
            if int(q[0]) > t: break
            cur = set(int(x, 16) for x in q[3:] if int(x, 16))
        return cur
    for a, st, en in exp:
        if en - st < 6: continue
        mid = (st + en) // 2
        want = {a} if a else set()
        if state_at(mid) != want: err("macro %s: at %d ms want %s got %s" % (steps, mid - t0, want, state_at(mid))); return
    if state_at(t + 50): err("macro %s: still held after the end" % steps)

def p_analog_shift_layer(r):
    g = r.randrange(4)                      # analog group: ls rs lp rp
    grp = [2, 3, 4, 5][g]                   # shift group
    base = GROUP[g][2]
    btn = r.choice(["L4", "R4", "L5"])
    p = Profile(); p.analog(g, PASS if g < 2 else MOUSE, speed=500)
    p.shift(grp, BTN[btn], mode=DPAD4, dz_in=2000)
    first = [0, 4, 8, 13, 18, 30, 42, 44, 46][grp]
    for k in range(first, first + 4):     # shifted up/down/left/right
        o = 1412 + 14 * k; p.b[o + 1] = KEYS[k - first]
    s = Sim(); s.use(p); s.run(5)
    put(s, g, 0, 30000); a = held(s)
    s.run(20, **{btn: 1}); put(s, g, 0, 30000); b = held(s); put(s, g, 30000, 0); c = held(s)
    s.run(20, **{btn: 0}); put(s, g, 30000, 0); d = held(s)
    put(s, g, 0, 0, touch=False)
    if a or b != {KEYS[0]} or c != {KEYS[3]} or d:
        err("analog shift g=%d btn=%s: normal %s shifted up %s right %s after %s" % (g, btn, a, b, c, d)); return
    if g < 2 and (s.run(5, **{GROUP[g][0]: 20000}) or True) and s.out[GROUP[g][0]] != 20000:
        err("analog shift g=%d: stick not passed again after the shift" % g)
    # layer replaces the stick / pad mode
    lay = Profile(); lay.analog(g, DPAD8, dz_in=2000)
    for i in range(4): lay.bind(base + i, KEYS[4 + i])
    bp = Profile(); bp.bind(btn, LAYER + 1); bp.analog(g, PASS if g < 2 else MOUSE, speed=500)
    s = Sim(); s.upload(1, lay); s.use(bp); s.run(5)
    s.run(20, **{btn: 1}); put(s, g, 0, -30000); e = held(s); s.run(20, **{btn: 0}); put(s, g, 0, -30000); f2 = held(s)
    if e != {KEYS[5]} or f2: err("analog layer g=%d: in layer %s after %s" % (g, e, f2))

ALL = [p_analog_shift_layer, p_dirs, p_gates_grid, p_radial, p_ring, p_hip, p_speeds, p_wheel, p_flick, p_deadzones, p_shift_layer, p_profiles, p_macro]
import ctypes
if __name__ == "__main__":
    a, b = int(sys.argv[1]), int(sys.argv[2])
    only = sys.argv[3:]
    for seed in range(a, b + 1):
        for f in ALL:
            if only and f.__name__ not in only: continue
            n = len(ERR)
            f(random.Random(seed * 100 + ALL.index(f)))
            for x in ERR[n:n + 1]: print("seed", seed, f.__name__, x)
    print("prop2 seeds %d-%d: %d problems" % (a, b, len(ERR)))
