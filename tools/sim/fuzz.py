"""Random profiles + random controller input; after letting go of everything nothing may stay pressed."""
import random, sys, struct, os, json
from simlib import *

STICK_MODES = [PASS, DPAD4, DPAD8, MOUSE, SCROLL, FLICK, RADIAL, SINGLE, XSTICK, REGION]
PAD_MODES = list(range(14))
ACTS = ([key(c) for c in "abcdefgw"] + [0xE0, 0xE1, MOUSE_L, MOUSE_R, MOUSE_M, 0x7B, WHEEL_UP, WHEEL_DOWN, 0x74,
        MOVE_UP, MOVE_RIGHT, PAD + 0, PAD + 9, PAD + 22, XBOX + 1, XBOX + 13, PROFILE + 0, PROFILE + 1, LAYER + 1, LAYER + 2,
        MACRO + 0, MACRO + 1, STICK + 0, STICK + 6, TRIG_L, TRIG_R, XTRIG_L, XTRIG_R])

def rnd_analog(r, pad):
    return dict(mode=r.choice(PAD_MODES if pad else STICK_MODES), flags=r.randrange(256) & (0xFF if pad else 0x66),
                dz_in=r.choice([0, 500, 1500, 3000, 6000]), dz_out=r.choice([0, 200, 1000, 3000]),
                speed=r.choice([0, 2, 4, 8, 30, 100, 1000, 3000]), haptic=r.randrange(4), rotate=r.choice([0, 0, 15, -30, 90]),
                curve=r.choice([0, 100, 50, 200, 300]), ring=r.choice([0, 0, 5000, 9000]), anti=r.choice([0, 0, 1000, 3000]),
                press=r.choice([0, 2000, 6000]))

def rnd_profile(r, name):
    p = Profile(name)
    p.flags(r.choice([0, 0, 1, 2, 3, 6]))
    p.dpad4(r.randrange(2))
    for t in range(2):
        p.trig_soft(t, r.choice([0, 10, 30, 60])); p.trig_mode(t, r.randrange(4), r.choice([0, 40, 150]))
        if r.random() < .3: struct.pack_into("<HH", p.b, 110 + 4 * t, r.choice([0, 1000]), r.choice([0, 5000, 9000]))
    p.times(r.choice([0, 150, 400]), r.choice([0, 150, 400]), r.choice([0, 30, 100]))
    p.move_speed(r.choice([0, 300, 3000]))
    for m in range(4):
        steps = [(r.choice([0] + ACTS[:12]), r.randrange(1, 30)) for _ in range(r.randrange(0, 6))]
        p.macro(m, steps)
    for i in range(4): p.analog(i, **rnd_analog(r, i >= 2))
    for g in range(8):
        if r.random() < .3:
            o = 328 + 20 * g; p.b[o] = r.randrange(1, 31); p.b[o + 1] = r.randrange(2)
            p.analog(0, base=o + 2, **rnd_analog(r, g in (4, 5)))
    def rb(off):
        mode = r.choice([0, 0, 0, M_HOLD, M_DOUBLE, M_START, M_RELEASE, M_TURBO, M_CYCLE, M_HOLD | M_DOUBLE, M_START | M_RELEASE, M_HOLD | M_TURBO])
        p.b[off] = mode
        for g in range(3):
            for k in range(4):
                p.b[off + 1 + g * 4 + k] = r.choice(ACTS) if (k == 0 or r.random() < .2) and r.random() < .7 else 0
        p.b[off + 13] = r.randrange(4) | ((r.randrange(1, 31) if r.random() < .1 else 0) << 2)
    for s in range(66):
        if r.random() < .5: rb(488 + 14 * s)
    for s in range(46):
        if r.random() < .3: rb(1412 + 14 * s)
    for k in range(30): struct.pack_into("<h", p.b, 44 + 2 * k, r.randrange(-1, 5))
    # v7 area
    if r.random() < .8:
        for k in range(124):
            if r.random() < .25: p.b[Profile.BT + 6 * k: Profile.BT + 6 * k + 6] = bytes([r.choice([0, 5, 30, 255]), r.choice([0, 10, 40]), r.choice([0, 1, 8, 255]), r.choice([0, 0, 3, 20]), r.choice([0, 0, 5, 30]), r.randrange(4)])
        for s2 in range(66, 78):
            if r.random() < .5: rb(Profile.XBIND + 14 * (s2 - 66))
        for k in range(12):
            if r.random() < .5: p.anx(k, scale_x=r.choice([0, 50, 300]), scale_y=r.choice([0, 25, 200]), edge_spin=r.choice([0, 0, 400, 65535]), slices=r.choice([0, 1, 2, 4, 8, 20]),
                                     flags=r.randrange(4), region=tuple(r.choice([0, 0, 1, 50, 100, 255]) for _ in range(4)))
        if r.random() < .6: p.gyx(mode=r.randrange(4), space=r.randrange(5), tilt_dz=r.choice([0, 1, 30, 90, 255]), tilt_8way=r.randrange(2),
                                  region=tuple(r.choice([0, 1, 50, 100, 255]) for _ in range(4)), region_deg=r.choice([0, 1, 60, 255]))
        for t in range(2): p.trig_curve(t, r.choice([0, 0, 1, 50, 300, 65535]))
        if r.random() < .3: p.screen(r.choice([0, 1, 640, 65535]), r.choice([0, 1, 480, 65535]))
    return p

NOSTEAMCHORD = (1 << 16)

def rnd_input(r, s):
    st = s.state
    b = st["buttons"]
    for _ in range(r.randrange(0, 3)):
        b ^= 1 << r.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 24, 28, 29])
    kw = {}
    if r.random() < .3: kw["lsx"], kw["lsy"] = r.choice([0, 32767, -32767, 15000, -8000]), r.choice([0, 32767, -32767, 20000, -5000])
    if r.random() < .3: kw["rsx"], kw["rsy"] = r.choice([0, 32767, -32767, 15000]), r.choice([0, 32767, -20000, 9000])
    for side, (t, c, x, y, pr) in enumerate([(25, 26, "lpx", "lpy", "lpp"), (21, 22, "rpx", "rpy", "rpp")]):
        if r.random() < .3:
            touch = r.random() < .7
            b = (b | 1 << t) if touch else (b & ~(1 << t) & ~(1 << c))
            if touch:
                kw[x], kw[y] = r.randrange(-32767, 32768), r.randrange(-32767, 32768)
                kw[pr] = r.choice([0, 3000, 20000])
                b = (b | 1 << c) if r.random() < .3 else (b & ~(1 << c))
            else:
                kw[x] = kw[y] = kw[pr] = 0
    for t, k, bit in ((0, "tl", 27), (1, "tr", 23)):
        if r.random() < .25:
            v = r.choice([0, 3000, 10000, 20000, 32767]); kw[k] = v
            b = (b | 1 << bit) if v > 30000 else (b & ~(1 << bit))
    if r.random() < .15:   # tilt / turn (gyro tilt, gyro region, player / world space)
        g = r.choice([(0, 0, 4096), (0, 4096, 0), (4096, 0, 0), (2896, 0, 2896), (0, -2896, 2896), (-4096, 0, 0), (0, 0, -4096)])
        kw["ax"], kw["ay"], kw["az"] = g
    if r.random() < .15: kw["gx"], kw["gy"], kw["gz"] = (r.choice([0, 0, 3000, -12000, 32767]) for _ in range(3))
    if (b & 0x84200) == 0x84200: b &= ~(1 << 14)   # not LB + RB + View: the gyro mouse toggle / calibration chord
    st["buttons"] = b
    s.set(**kw)

def neutral(s):
    for k in s.state: s.state[k] = 0

def check_idle(s, tag):
    ev = s.run(12000)  # macros (<= 5 steps x 290 ms), turbo, trackball, flick all finish
    tail = s.run(1500)
    probs = []
    ks = Events(s.log).kbd()
    if ks and ks[-1] != (): probs.append("keys stuck %s" % (ks[-1],))
    if s.lib.sim_mouse_buttons(): probs.append("mouse buttons stuck %d" % s.lib.sim_mouse_buttons())
    if s.lib.sim_xbox_buttons(): probs.append("xbox buttons stuck %x" % s.lib.sim_xbox_buttons())
    if s.out["buttons"]: probs.append("pad buttons stuck %x" % s.out["buttons"])
    for k in ("lsx", "lsy", "rsx", "rsy", "tl", "tr"):
        if s.out[k]: probs.append("%s=%d at rest" % (k, s.out[k]))
    mv = tail.mouse()
    if any(m[1] or m[2] or m[3] or m[4] for m in mv): probs.append("mouse still moving %s" % (mv[:3],))
    if tail.haptics(): probs.append("haptics while idle %s" % tail.haptics()[:2])
    return probs

VALID_KEYS = set(range(0x04, 0x66)) | set(range(0xE0, 0xE8))

def extra_checks(r, seed, found):
    """remapper off = identity; an empty (all pass) profile = identity; profile round trip"""
    s = Sim()
    p = rnd_profile(r, "rt"); s.upload(2, p)
    if s.download(2) != bytes(p.b): found.append((seed, "rt", 0, "profile read back differs"))
    s.use(p, 2); s.cfg("remap_on", 0); s.run(5)
    for _ in range(300):
        rnd_input(r, s)
        if (s.state["buttons"] & 0x80240) == 0x80240: s.state["buttons"] &= ~0x40  # not the LB+RB+Menu chord
        if s.state["buttons"] & (1 << 16): s.state["buttons"] &= ~((1 << 10) | (1 << 11) | (1 << 12) | (1 << 13))
        e = s.run(r.choice([1, 5, 30]))
        if e.kbd() or e.mouse(): found.append((seed, "off", 0, "remapper off but output %s" % e[:2])); break
        if s.out != s.state: found.append((seed, "off", 0, "remapper off changes the pad report %s" % {k: (s.state[k], s.out[k]) for k in s.out if s.out[k] != s.state[k]})); break
    s = Sim(); s.use(Profile("empty"), 0); s.run(5)
    for _ in range(300):
        rnd_input(r, s)
        if (s.state["buttons"] & 0x80240) == 0x80240: s.state["buttons"] &= ~0x40
        if s.state["buttons"] & (1 << 16): s.state["buttons"] &= ~((1 << 10) | (1 << 11) | (1 << 12) | (1 << 13))
        e = s.run(r.choice([1, 5, 30]))
        if e.kbd() or e.mouse(): found.append((seed, "empty", 0, "empty profile but output %s" % e[:2])); break
        want = dict(s.state); want["buttons"] &= ~(1 << 16)  # the Steam button is hidden while the remapper is on
        if s.out != want: found.append((seed, "empty", 0, "empty profile changes the pad report %s" % {k: (want[k], s.out[k]) for k in s.out if s.out[k] != want[k]})); break

SIMPLE_ACTS = [key(c) for c in "abcdw"] + [0xE1, MOUSE_L, MOUSE_R, WHEEL_UP, MOVE_UP]

def hidden_checks(r, seed, found, n=20):
    """simple profiles (no shifts/layers/pad actions): a bound button never reaches Steam,
    an unbound one always does; a stick not 'to Steam' is silent"""
    for t in range(n):
        p = Profile("h")
        bound = 0
        for b in range(20):
            if b in (6, 9, 19, 16, 10, 11, 12, 13): continue  # chord / Steam / D-pad have system uses
            if r.random() < .4:
                p.bind(b, r.choice(SIMPLE_ACTS), mode=r.choice([0, M_HOLD, M_START, M_TURBO])); bound |= 1 << b
        modes = [r.choice(STICK_MODES), r.choice(STICK_MODES)]
        for i in range(2): p.analog(i, modes[i], dz_in=r.choice([0, 2000]))
        s = Sim(); s.use(p); s.run(5)
        for _ in range(200):
            rnd_input(r, s)
            s.state["buttons"] &= ~((1 << 6) | (1 << 9) | (1 << 19) | (1 << 16) | (0xF << 10))
            s.run(r.choice([1, 5, 30]))
            ib, ob = s.state["buttons"] & 0xFFFFF, s.out["buttons"] & 0xFFFFF
            if ob != ib & ~bound:
                found.append((seed, "hidden", t, "pad buttons in %x out %x bound %x" % (ib, ob, bound))); return
            for i, (kx, ky) in enumerate((("lsx", "lsy"), ("rsx", "rsy"))):
                if modes[i] != PASS and (s.out[kx] or s.out[ky]):
                    found.append((seed, "hidden", t, "stick %d mode %d still sent %d,%d" % (i, modes[i], s.out[kx], s.out[ky]))); return
                if modes[i] == PASS and (s.out[kx], s.out[ky]) != (s.state[kx], s.state[ky]) and p.b[256 + 18 * i + 2] == 0:
                    found.append((seed, "hidden", t, "stick %d pass changed %s" % (i, (s.state[kx], s.out[kx])))); return

def round_(seed, profiles=40, seqs=6, steps=150):
    r = random.Random(seed)
    found = []
    extra_checks(r, seed, found)
    hidden_checks(random.Random(seed * 7919), seed, found)
    for pi in range(profiles):
        s = Sim()
        profs = [rnd_profile(r, "f%d" % k) for k in range(3)]
        for k in range(1, 3): s.upload(k, profs[k])
        s.use(profs[0], 0); s.run(5)
        hist = []
        for q in range(seqs):
            for _ in range(steps):
                rnd_input(r, s)
                hist.append((s.lib.sim_time(), dict(s.state)))
                e = s.run(r.choice([1, 2, 5, 10, 20, 40, 80, 150, 300]))
                for k in e.kbd():
                    if len([c for c in k if c < 0xE0]) > 6: found.append((seed, pi, q, "more than 6 keys"))
                    if any(c not in VALID_KEYS for c in k): found.append((seed, pi, q, "bad key code %s" % (k,)))
                    if len(set(k)) != len(k): found.append((seed, pi, q, "same key twice %s" % (k,)))
                for k in ("lsx", "lsy", "rsx", "rsy"):
                    if not -32767 <= s.out[k] <= 32767: found.append((seed, pi, q, "%s out of range %d" % (k, s.out[k])))
                for k in ("tl", "tr"):
                    if not 0 <= s.out[k] <= 32767: found.append((seed, pi, q, "%s out of range %d" % (k, s.out[k])))
            neutral(s)
            probs = check_idle(s, (seed, pi, q))
            if probs and os.environ.get("FUZZDBG"):
                print("DBG", probs, "gm cfg", {k: s.cfg_get(k) for k in ("gm_enabled", "gm_output", "gm_space", "remap_profile", "remap_on")})
                for k, v in (("gm_enabled", 0),):
                    s.cfg(k, v); print(" after", k, v, Events(s.run(500)).mouse()[:3])
                s.cfg("gm_enabled", 1)
                for sp in (0, 1, 2):
                    s.cfg("gm_space", sp); print(" space", sp, Events(s.run(300)).mouse()[:2])
                s.cfg("remap_on", 0); print(" remap off", Events(s.run(300)).mouse()[:2])
            if probs:
                found.append((seed, pi, q, "; ".join(probs)))
                fn = "/tmp/fuzzfail_%d_%d_%d.json" % (seed, pi, q)
                json.dump({"profiles": [p.b.hex() for p in profs], "seed": seed, "hist": hist[-60:], "log": s.log[-400:]}, open(fn, "w"))
                break
    return found

if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    f = round_(seed, n)
    for x in f[:15]: print(x)
    print("seed %d: %d problems" % (seed, len(f)))
