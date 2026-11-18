"""Exact timing properties of the binding modes, swept 1 ms at a time across every boundary,
with random profile times; checked at the remapper output and at the PC (USB / Puck / BT)."""
import random, sys
from simlib import *

A, B, C = key("a"), key("b"), key("c")
TRANSPORTS = {"usb": None, "puck": (1, 1, 20, 15), "bt": (2, 0, 10, 15)}

def mk(mode, H=0, W=0, T=0):
    p = Profile(); p.times(H, W, T); p.bind("A", A, B, C, mode=mode); return p

def sim(p, tr):
    s = Sim()
    if tr: s.transport(*tr, seed=7)
    s.use(p); s.run(300)
    s.log = []
    return s

def presses(s, code, host):
    n, was = 0, False
    tag = "HK" if host else "K"
    for e in s.log:
        q = e.split()
        if q[1] != tag: continue
        now = ("%02x" % code) in q[3:]
        if now and not was: n += 1
        was = now
    return n

def pattern(s, seq):
    for down, ms in seq:
        s.run(ms, A=down)
    s.run(1500, A=0)

def check(name, p, seq, want, tr_name, errs):
    s = sim(p, TRANSPORTS[tr_name])
    pattern(s, seq)
    for host in ((False, True) if tr_name != "usb" else (False,)):
        got = {k: presses(s, c, host) for k, c in (("a", A), ("b", B), ("c", C))}
        if got != want:
            errs.append("%s [%s%s] seq=%s want %s got %s" % (name, tr_name, " PC" if host else "", seq, want, got))
            return

def run(seed, tr_name):
    r = random.Random(seed)
    errs = []
    H = r.choice([0, 100, 150, 250, 333, 600]); He = H or 250
    W = r.choice([0, 100, 180, 250, 400]); We = W or 250
    T = r.choice([0, 20, 35, 100, 250]); Te = max(T or 100, 20)
    # plain: one press per press
    for d in (1, 2, 5, 40, 300):
        check("plain d=%d" % d, mk(0, H, W, T), [(1, d)], {"a": 1, "b": 0, "c": 0}, tr_name, errs)
    # long press
    for d in range(He - 3, He + 4):
        want = {"a": 1, "b": 0, "c": 0} if d <= He else {"a": 0, "b": 1, "c": 0}
        check("hold H=%d d=%d" % (He, d), mk(M_HOLD, H, W, T), [(1, d)], want, tr_name, errs)
    # double press: gap across the window
    for g in range(We - 3, We + 4):
        want = {"a": 0, "b": 0, "c": 1} if g <= We else {"a": 2, "b": 0, "c": 0}
        check("double W=%d gap=%d" % (We, g), mk(M_DOUBLE, H, W, T), [(1, 30), (0, g), (1, 30)], want, tr_name, errs)
    # hold + double
    d1 = r.choice([20, max(1, He - 5)])
    for g in (We - 2, We + 2):
        want = {"a": 0, "b": 0, "c": 1} if g <= We else {"a": 2, "b": 0, "c": 0}
        check("hold+double gap=%d" % g, mk(M_HOLD | M_DOUBLE, H, W, T), [(1, d1), (0, g), (1, 20)], want, tr_name, errs)
    check("hold+double long", mk(M_HOLD | M_DOUBLE, H, W, T), [(1, He + 20)], {"a": 0, "b": 1, "c": 0}, tr_name, errs)
    # start + release
    for d in (1, 3, 50, He + 10):
        check("start+release d=%d" % d, mk(M_START | M_RELEASE, H, W, T), [(1, d)], {"a": 1, "b": 0, "c": 1}, tr_name, errs)
    # start + hold
    check("start+hold", mk(M_START | M_HOLD, H, W, T), [(1, He + 20)], {"a": 1, "b": 1, "c": 0}, tr_name, errs)
    # toggle: odd number of presses leaves it on until the next one
    check("toggle x2", mk(M_TOGGLE, H, W, T), [(1, 20), (0, 50), (1, 20)], {"a": 1, "b": 0, "c": 0}, tr_name, errs)
    # cycle
    seq = []
    n = r.randrange(1, 8)
    for _ in range(n): seq += [(1, 20), (0, 60)]
    want = {"a": (n + 2) // 3, "b": (n + 1) // 3, "c": n // 3}
    check("cycle n=%d" % n, mk(M_CYCLE, H, W, T), seq, want, tr_name, errs)
    # turbo: presses while held (first at once, then every period)
    d = r.choice([Te * 3 + Te // 2, Te * 7 + Te // 2, 999])
    if tr_name == "usb":
        s = sim(mk(M_TURBO, H, W, T), None); pattern(s, [(1, d)])
        n = presses(s, A, False); exp = d // Te + 1
        if abs(n - exp) > 1: errs.append("turbo T=%d d=%d: %d presses, expected about %d" % (Te, d, n, exp))
    elif Te >= 50:  # over radio: every turbo press must reach the PC
        s = sim(mk(M_TURBO, H, W, T), TRANSPORTS[tr_name]); pattern(s, [(1, d)])
        a, b = presses(s, A, False), presses(s, A, True)
        if a != b: errs.append("turbo over %s: %d sent, %d at the PC" % (tr_name, a, b))
    return errs

if __name__ == "__main__":
    first, last = int(sys.argv[1]), int(sys.argv[2])
    total = 0
    for seed in range(first, last + 1):
        for tr in TRANSPORTS:
            e = run(seed, tr)
            total += len(e)
            for x in e[:4]: print("seed", seed, x)
    print("prop seeds %d-%d: %d problems" % (first, last, total))
