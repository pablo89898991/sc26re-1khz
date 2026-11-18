"""Record a failing fuzz case as (profiles, steps) and shrink it to a minimal repro."""
import random, json, sys, copy
import fuzz
from simlib import *

def record(seed, n=40):
    r = random.Random(seed)
    fuzz.extra_checks(r, seed, [])
    for pi in range(n):
        s = Sim()
        profs = [fuzz.rnd_profile(r, "f%d" % k) for k in range(3)]
        for k in range(1, 3): s.upload(k, profs[k])
        s.use(profs[0], 0); s.run(5)
        steps = []
        for q in range(6):
            for _ in range(150):
                fuzz.rnd_input(r, s); ms = r.choice([1, 2, 5, 10, 20, 40, 80, 150, 300])
                steps.append((dict(s.state), ms)); s.run(ms)
            fuzz.neutral(s)
            pr = fuzz.check_idle(s, 0)
            if pr: return [p.b for p in profs], steps, pr
            steps.append((dict(s.state), 13500))
    return None

KIND = None
def kind(pr): return sorted(set(p.split()[0] + p.split()[1] for p in pr))
def fails(profs, steps):
    pr = _fails(profs, steps)
    return pr and (KIND is None or kind(pr) == KIND)
def _fails(profs, steps):
    s = Sim()
    for k in (1, 2): s.upload(k, Profile.__new__(Profile)) if False else None
    for k in (1, 2):
        p = Profile(); p.b = bytearray(profs[k]); s.upload(k, p)
    p = Profile(); p.b = bytearray(profs[0]); s.use(p, 0); s.run(5)
    for st, ms in steps:
        s.state = dict(st); s.run(ms)
    fuzz.neutral(s)
    return fuzz.check_idle(s, 0)

def shrink(profs, steps):
    profs = [bytearray(p) for p in profs]
    # 1. steps: drop chunks
    ch = len(steps) // 2
    while ch >= 1:
        i = 0
        while i < len(steps):
            t = steps[:i] + steps[i + ch:]
            if t and fails(profs, t): steps = t
            else: i += ch
        ch //= 2
    # 2. bindings / shifted bindings / analog / shift groups / macros: zero what is not needed
    regions = [(488 + 14 * s, 14) for s in range(66)] + [(1412 + 14 * s, 14) for s in range(46)] + \
              [(328 + 20 * g, 20) for g in range(8)] + [(128 + 32 * m, 32) for m in range(4)]
    for k in (2, 1, 0):
        for o, n in regions:
            if not any(profs[k][o:o + n]): continue
            t = [bytearray(p) for p in profs]; t[k][o:o + n] = bytes(n)
            if fails(t, steps): profs = t
        # single action bytes
        for o, n in regions[:112]:
            for b in range(o + 1, o + 13):
                if profs[k][b]:
                    t = [bytearray(p) for p in profs]; t[k][b] = 0
                    if fails(t, steps): profs = t
        for o, v in ((17, 0), (16, 0)):
            t = [bytearray(p) for p in profs]; t[k][o] = v
            if fails(t, steps): profs = t
        for i in range(4):  # analog -> pass
            o = 256 + 18 * i
            t = [bytearray(p) for p in profs]; t[k][o:o + 18] = bytes(18)
            if fails(t, steps): profs = t
    # 3. steps again, then simplify each step's state
    ch = max(1, len(steps) // 2)
    while ch >= 1:
        i = 0
        while i < len(steps):
            t = steps[:i] + steps[i + ch:]
            if t and fails(profs, t): steps = t
            else: i += ch
        ch //= 2
    for i in range(len(steps)):
        st, ms = steps[i]
        for bit in range(30):
            if st["buttons"] >> bit & 1:
                t = copy.deepcopy(steps); t[i][0]["buttons"] &= ~(1 << bit)
                if fails(profs, t): steps = t
        for k in list(st):
            if k != "buttons" and steps[i][0][k]:
                t = copy.deepcopy(steps); t[i][0][k] = 0
                if fails(profs, t): steps = t
    return profs, steps

def describe(profs, steps):
    inv = {v: k for k, v in SRC.items()}; binv = {v: k for k, v in BTN.items()}
    out = []
    for k, p in enumerate(profs):
        lines = []
        for s in range(66):
            o = 488 + 14 * s
            if any(p[o:o + 14]): lines.append("  bind %s mode=%d acts=%s hap=%d" % (inv.get(s, s), p[o], [hex(x) for x in p[o + 1:o + 13] if x], p[o + 13]))
        for s in range(46):
            o = 1412 + 14 * s
            if any(p[o:o + 14]): lines.append("  shifted %s mode=%d acts=%s" % (inv.get(SHIFT_SRC[s]), p[o], [hex(x) for x in p[o + 1:o + 13] if x]))
        for g in range(8):
            o = 328 + 20 * g
            if any(p[o:o + 20]): lines.append("  shift group %d button=%s an=%s" % (g, binv.get(p[o] - 1), list(p[o + 2:o + 6])))
        for i in range(4):
            o = 256 + 18 * i
            if any(p[o:o + 18]): lines.append("  analog %d mode=%d flags=%x dz=%d" % (i, p[o], p[o + 1], p[o + 2] | p[o + 3] << 8))
        for m in range(4):
            o = 128 + 32 * m
            if any(p[o:o + 32]): lines.append("  macro %d %s" % (m, list(p[o:o + 12])))
        lines.append("  flags=%x dpad4=%d trig_mode=%s" % (p[17], p[16], list(p[104:106])))
        out.append("profile %d:\n" % k + "\n".join(lines))
    out.append("steps:")
    for st, ms in steps:
        out.append("  %5d ms  %s %s" % (ms, [binv[i] for i in range(30) if st["buttons"] >> i & 1], {k: v for k, v in st.items() if v and k != "buttons"}))
    return "\n".join(out)

if __name__ == "__main__":
    global_kind = None
    seed = int(sys.argv[1])
    rec = record(seed, int(sys.argv[2]) if len(sys.argv) > 2 else 40)
    if not rec: print("no failure"); sys.exit()
    profs, steps, pr = rec
    print("failure:", pr, "steps", len(steps))
    globals()["KIND"] = kind(pr)
    profs, steps = shrink(profs, steps)
    print("after shrink:", _fails(profs, steps))
    print(describe(profs, steps))
    json.dump({"profs": [p.hex() for p in profs], "steps": steps}, open("/tmp/min_%d.json" % seed, "w"))
