import random, sys, json, copy
import fuzz, wfuzz, shrink
from simlib import *

def record(seed, mode, target):
    r = random.Random(seed * 1000 + mode)
    for pi in range(target + 1):
        tr = None
        if mode == 1: tr = (1, wfuzz.ESB_ONE, r.choice([0, 5, 30, 100]), 15, r.randrange(1 << 30))
        if mode == 2: tr = (2, 0, r.choice([0, 5, 30]), r.choice([8, 15, 30]), r.randrange(1 << 30))
        profs = [fuzz.rnd_profile(r, "w%d" % k) for k in range(3)]
        s = Sim()
        if tr: s.transport(*tr)
        for k in (1, 2): s.upload(k, profs[k])
        s.use(profs[0], 0); s.run(5)
        steps = []
        for i in range(400):
            for ch, ms in wfuzz.complex_input(r, s, profs[0]):
                wfuzz.apply(s, ch)
                if ms: steps.append((dict(s.state), ms)); s.run(ms)
                else: steps.append((dict(s.state), 0))
        if pi == target: return tr, [p.b for p in profs], steps

TR = None
def run(profs, steps):
    s = Sim()
    if TR: s.transport(*TR)
    for k in (1, 2):
        p = Profile(); p.b = bytearray(profs[k]); s.upload(k, p)
    p = Profile(); p.b = bytearray(profs[0]); s.use(p, 0); s.run(5)
    for st, ms in steps:
        s.state = dict(st)
        if ms: s.run(ms)
    fuzz.neutral(s); s.run(15000)
    ks = Events(s.log).kbd(); h = s.host()
    pr = []
    if ks and ks[-1] != (): pr.append("device keys stuck")
    if h["kbd"] != bytes(8): pr.append("PC keys stuck")
    return pr

if __name__ == "__main__":
    seed, mode, target = map(int, sys.argv[1:4])
    TR, profs, steps = record(seed, mode, target)
    shrink._fails = run
    pr = run(profs, steps); print("failure:", pr)
    shrink.KIND = shrink.kind(pr)
    profs, steps = shrink.shrink(profs, [s for s in steps])
    print("after:", run(profs, steps))
    print(shrink.describe(profs, [s for s in steps if s[1]]))
