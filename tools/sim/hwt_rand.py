"""Random hardware test: random profiles (fuzz.rnd_profile) and random input scripts, run on the
controller by hwtest.html?set=hwrand and compared with the simulator.

    python3 hwt_rand.py export [n] [seed]   -> web/hwrand.json
    python3 hwt_rand.py self   [n] [seed]   -> the simulator against itself (must be 0 bad)

Kept off the controller: profile switches and layers (they would read the user's other profiles),
the Steam button (stops the test), the calibration chord (any 3 of LB / RB / X / Y) and the gyro
mouse chord (LB + RB + View), the remapper on / off chord (LB + RB + Menu); the profile's own gyro settings are cleared (the gyro mouse is off
for the run anyway)."""
import json, os, random, struct, sys
import fuzz, hwt
from simlib import *

HERE = os.path.dirname(os.path.abspath(__file__))
SAFE_KEYS = [key(c) for c in "hijklmnop"]
B = BTN
CHORD3 = [B["LB"], B["RB"], B["X"], B["Y"]]
BTN_BITS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 24, 28, 29]


def sanitize(p, r):
    """no profile switches / layers anywhere; no own gyro settings"""
    def fix(v):
        if PROFILE <= v < LAYER + 4: return r.choice(SAFE_KEYS)
        return v
    # bindings: 66 + 46 + 12 slots of 14 bytes (mode + 3 sets x 4 actions + flags)
    for base, n in ((488, 66), (1412, 46), (Profile.XBIND, 12)):
        for s in range(n):
            o = base + 14 * s
            for k in range(12): p.b[o + 1 + k] = fix(p.b[o + 1 + k])
    p.b[17] &= ~(PF_GYRO | PF_GYRO_ON)
    for k in range(30): struct.pack_into("<h", p.b, 44 + 2 * k, -1)
    return p


def rnd_steps(r, n):
    """n steps of random input (fuzz.rnd_input's choices), each held 15..160 ms"""
    class S0: pass
    s = S0(); s.state = dict(buttons=0)
    s.set = lambda **kw: s.state.update(kw)
    steps = [(60, {})]
    for _ in range(n - 2):
        st = s.state
        fuzz.rnd_input(r, s)
        b = st["buttons"] & ~(1 << B["STEAM"])
        while sum(1 for x in CHORD3 if b >> x & 1) >= 3:   # any 3 of them: the calibration chord
            b &= ~(1 << r.choice([x for x in CHORD3 if b >> x & 1]))
        if (b >> B["LB"] & 1) and (b >> B["RB"] & 1):
            b &= ~(1 << B["VIEW"]) & ~(1 << B["MENU"])   # gyro mouse chord / remapper on-off chord
        st["buttons"] = b
        steps.append((r.choice([15, 25, 40, 70, 110, 160]), {k: v for k, v in st.items() if v}))
    steps.append((250, {}))   # everything let go
    return steps


def robust(p, steps):
    """the same outputs when every step is 2 ms longer or shorter (the controller's steps jitter by a
    millisecond or two; a case that hangs on that would only show timing, not bugs)"""
    def mk(st):
        return dict(profile=bytes(p.b).hex(), script=hwt.script_bytes(st).hex(), ms=sum(ms for ms, _ in st), snap=True, slot=hwt.SLOT, extra={})
    base = hwt.sim_run(mk(steps))
    if base["flags"] & 8: return False
    for d in (2, -2):
        alt = [(max(5, ms + d), x) for ms, x in steps]
        if hwt.same(hwt.sim_run(mk(alt))["log"], base["log"], 8, True, 0.2): return False
    return True


def cases(n, seed):
    out = []
    i = 0
    while len(out) < n:
        r = random.Random(seed * 1000 + i); i += 1
        p = sanitize(fuzz.rnd_profile(r, "rnd%d" % i), r)
        steps = rnd_steps(r, r.randrange(12, hwt.MAX_STEPS + 1))
        if not robust(p, steps): continue
        out.append(dict(name="rnd_%d_%d" % (seed, i), what="random profile + random input (seed %d / %d)" % (seed, i),
                        profile=bytes(p.b).hex(), script=hwt.script_bytes(steps).hex(), ms=sum(ms for ms, _ in steps),
                        snap=True, tol=8, rel=0.2, slot=hwt.SLOT, extra={}))
    return out


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "self"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    cs = cases(n, seed)
    if cmd == "export":
        for c in cs: c["sim"] = hwt.sim_run(c)["log"]
        doc = dict(version=2, slot=hwt.SLOT, cfg=hwt.cfg_names(), cases=cs)
        path = os.path.join(HERE, "../../web/hwrand.json")
        json.dump(doc, open(path, "w"), separators=(",", ":"))
        print("wrote %s: %d cases" % (path, len(cs)))
    else:
        bad = 0
        for c in cs:
            a, b = hwt.sim_run(c), hwt.sim_run(c)
            P = hwt.same(a["log"], b["log"], c["tol"], c["snap"], c["rel"])
            if a["flags"] & 8: P.insert(0, "log full")
            bad += bool(P)
            print(("ok  " if not P else "BAD ") + "%-14s %3d entries %5d ms" % (c["name"], len(a["log"]), c["ms"]))
            for x in P[:3]: print("    " + x)
        print("%d bad" % bad)


if __name__ == "__main__":
    main()
