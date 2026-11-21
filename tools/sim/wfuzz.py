"""Complex sequences, over USB / Puck / Bluetooth. What the PC receives must match what the
remapper meant to send: same presses of every key and mouse button, same movement, nothing stuck."""
import random, sys, json
import fuzz
from simlib import *

import ctypes
GYRO = os.environ.get("SIMLIB", "").startswith("libsim_gm")
PAGE = True
STABLE = True  # page actions in the middle of play (remapper switch, profile choice, profile save)

def page_action(r, s):
    k = r.random()
    if k < .35: s.cfg("remap_on", r.randrange(2))
    elif k < .65: s.cfg("remap_profile", r.randrange(4))
    else:
        import fuzz as _f
        slot = r.randrange(4)
        p = _f.rnd_profile(r, "pg")
        b = p.b
        for o in range(0, len(b), 40):
            s.feature(0xEE, "sc26re/rmp/%d/%x" % (slot, o), b[o:o + 40])
            if r.random() < .3: s.run(r.choice([1, 3, 8]))  # the page sends while the game runs
        if r.random() < .9: s.feature(0xEF, "sc26re/rmp/%d" % slot)  # (sometimes cut off)

def complex_input(r, s, prof):
    """bursts of fast presses, presses timed around the hold / double windows, combos, shift and layer buttons mid-press"""
    st = s.state
    if PAGE and r.random() < .01:
        page_action(r, s)
        return [({}, 1)]
    kind = r.random()
    if kind < .25:      # rapid burst on one button
        b = r.choice([0, 1, 2, 3, 7, 8, 17, 18, 5, 15])
        seq = []
        for _ in range(r.randrange(2, 8)):
            seq += [({"bit": b, "v": 1}, r.choice([1, 2, 3, 8, 30])), ({"bit": b, "v": 0}, r.choice([1, 2, 3, 8, 30]))]
        return seq
    if kind < .45:      # around the timing windows
        b = r.choice([0, 1, 2, 3, 17, 18, 7, 8])
        hold = r.choice([150, 250, 400, 600]); d = r.choice([-6, -1, 0, 1, 6])
        return [({"bit": b, "v": 1}, max(1, hold + d)), ({"bit": b, "v": 0}, r.choice([20, 140, 249, 251, 400]))]
    if kind < .6:       # chord: many buttons in the same millisecond, then released in another order
        bits = r.sample([0, 1, 2, 3, 5, 7, 8, 9, 15, 17, 18, 19, 20, 24, 28, 29], r.randrange(2, 7))
        seq = [({"bit": b, "v": 1}, 0) for b in bits[:-1]] + [({"bit": bits[-1], "v": 1}, r.choice([1, 20, 300]))]
        r.shuffle(bits)
        seq += [({"bit": b, "v": 0}, r.choice([0, 0, 1, 5])) for b in bits]
        return seq
    fuzz.rnd_input(r, s)
    apply(s, {})
    return [({}, r.choice([1, 2, 5, 10, 20, 40, 80, 150, 300]))]

def apply(s, ch):
    if "bit" in ch:
        b = 1 << ch["bit"]
        s.state["buttons"] = (s.state["buttons"] | b) if ch["v"] else (s.state["buttons"] & ~b)
    if GYRO and s.state["buttons"] & (1 << 16):
        s.state["buttons"] &= ~((1 << 14) | (1 << 0))  # no Steam + View / A: the gyro's own combinations (the 360 calibration turns on purpose)

def edges(evs, prefix):
    """count presses per key / mouse button, from 'K'/'M' (device) or 'HK'/'HM' (host) lines"""
    cnt, prev_k, prev_m = {}, set(), 0
    for e in evs:
        p = e.split()
        if p[1] == prefix + "K":
            mods = int(p[2], 16)
            cur = set([0xE0 + i for i in range(8) if mods >> i & 1] + [int(x, 16) for x in p[3:] if int(x, 16)])
            for k in cur - prev_k: cnt[k] = cnt.get(k, 0) + 1
            prev_k = cur
        elif p[1] == prefix + "M":
            mb = int(p[2])
            for i in range(5):
                if mb >> i & 1 and not prev_m >> i & 1: cnt["m%d" % i] = cnt.get("m%d" % i, 0) + 1
            prev_m = mb
    return cnt

def mouse_sum(evs, prefix):
    t = [0, 0, 0, 0]
    for e in evs:
        p = e.split()
        if p[1] == prefix + "M":
            for i in range(4): t[i] += int(p[3 + i])
    return t

import os
ESB_ONE = int(os.environ.get("ESB_ONE", "1"))  # 1 = the fixed firmware (one report per payload, merge only same buttons)

def scenario(seed, mode, n_profiles=20, steps=400, disconnects=False):
    r = random.Random(seed * 1000 + mode)
    found = []
    for pi in range(n_profiles):
        s = Sim()
        if mode == 1: s.transport(1, ESB_ONE, loss=r.choice([0, 5, 30, 100]), seed=r.randrange(1 << 30))
        if mode == 2: s.transport(2, 0, loss=r.choice([0, 5, 30]), ci=r.choice([8, 15, 30]), seed=r.randrange(1 << 30))
        if mode and disconnects: s.jitter = lambda: r.choice([1, 1, 1, 2, 3])
        profs = [fuzz.rnd_profile(r, "w%d" % k) for k in range(3)]
        for k in (1, 2): s.upload(k, profs[k])
        s.use(profs[0], 0)
        gyro = hasattr(s.lib, "sim_gyro") and GYRO
        if gyro:  # the real gyro_mouse.c: random activation (also bound / touch inputs), mode, output
            s.lib.sim_gyro.argtypes = [ctypes.c_float] * 3
            s.cfg("gm_enabled", 1); s.cfg("gm_act_button", r.choice([-1, 20, 24, 28, 7, 17, 9]))
            s.cfg("gm_act_mode", r.randrange(4)); s.cfg("gm_output", r.choice([0, 0, 1, 3]))
            s.cfg("gm_ratchet", r.choice([-1, 8, 18]))
        s.run(5)
        # what the start sent (a mouse region moves the pointer to its place) is delivered before counting
        # starts: over Bluetooth a 16384 px wide region takes ~3 s, the 2 s waited here before left the
        # rest in the radio queue, and it counted as received but never sent (seed 9818 BT)
        m = (ctypes.c_float * 16)()
        for _ in range(4000):
            s.lib.sim_mouse_state(m)
            if not (s.host()["queued"] or any(abs(m[k]) >= 1 for k in (0, 1, 6, 7))): break
            s.run(5)
        s.log = []
        down_until = -1
        for i in range(steps):
            if gyro and r.random() < .05: s.lib.sim_gyro(r.uniform(-300, 300), r.uniform(-300, 300), r.uniform(-100, 100))
            for ch, ms in complex_input(r, s, profs[0]):
                apply(s, ch)
                if ms: s.run(ms)
            if STABLE and r.random() < .015:
                # stability: the controller held still for 3 s, then nothing may keep flipping
                s.run(3000)
                n0 = len(s.log); s.run(1000); tail = s.log[n0:]
                flips = [e for e in tail if e.split()[1] in ("K",) or (e.split()[1] == "M" and e.split()[2] != (tail[0].split()[2] if False else e.split()[2]))]
                kb = [e for e in tail if e.split()[1] == "K"]
                mb = set(e.split()[2] for e in tail if e.split()[1] == "M")
                padflip = s.out["buttons"]
                s.run(1); padflip2 = s.out["buttons"]
                turbo = any(p.b[488 + 14 * k] & M_TURBO for p in profs for k in range(66)) or any(p.b[1412 + 14 * k] & M_TURBO for p in profs for k in range(46))
                cyc = any(p.b[488 + 14 * k] & (M_TURBO) for p in profs for k in range(66))
                if not turbo and (len(kb) > 4 or len(mb) > 1):
                    found.append((seed, mode, pi, "output keeps changing with the controller held still: %s" % (kb[:3] or list(mb))))
                    json.dump({"profs": [p.b.hex() for p in profs]}, open("/tmp/wstable_%d_%d_%d.json" % (seed, mode, pi), "w"))
                    break
            if disconnects and mode and r.random() < .01:
                s.link(False); s.run(r.choice([30, 300, 2000])); s.link(True)
        fuzz.neutral(s)
        if gyro: s.lib.sim_gyro(0, 0, 0)
        s.run(15000)
        # a mouse region over the radio may still be on its way (a big screen): let it arrive
        m = (ctypes.c_float * 16)()
        for _ in range(12):
            s.lib.sim_mouse_state(m)
            if not (s.host()["queued"] or any(abs(m[k]) >= 1 for k in (0, 1, 6, 7))): break
            s.run(5000)
        h = s.host()
        dev = [e for e in s.log if e.split()[1] in ("K", "M")]
        host = [e for e in s.log if e.split()[1] in ("HK", "HM")]
        probs = []
        if h["kbd"] != bytes(8): probs.append("PC: keys stuck %s" % h["kbd"].hex())
        if h["mb"]: probs.append("PC: mouse button stuck %d" % h["mb"])
        if h["queued"]: probs.append("reports still queued %d" % h["queued"])
        ks = Events(s.log).kbd()
        if ks and ks[-1] != (): probs.append("device keys stuck %s" % (ks[-1],))
        if not disconnects:
            ed, eh = edges(dev, ""), edges(host, "H")
            diff = {k: (ed.get(k, 0), eh.get(k, 0)) for k in set(ed) | set(eh) if ed.get(k, 0) != eh.get(k, 0)}
            if diff: probs.append("PC got different presses (sent, received): %s" % dict(list(diff.items())[:6]))
            md, mh = mouse_sum(dev, ""), mouse_sum(host, "H")
            if md != mh:
                probs.append("mouse movement sent %s received %s" % (md, mh))
                open("/tmp/wmouse_%d_%d_%d.log" % (seed, mode, pi), "w").write("\n".join(s.log))
        if probs:
            found.append((seed, mode, pi, "; ".join(probs)))
            json.dump({"profs": [p.b.hex() for p in profs]}, open("/tmp/wfail_%d_%d_%d.json" % (seed, mode, pi), "w"))
    return found

if __name__ == "__main__":
    seed, mode = int(sys.argv[1]), int(sys.argv[2])
    disc = len(sys.argv) > 3 and sys.argv[3] == "d"
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 20
    f = scenario(seed, mode, n, disconnects=disc)
    for x in f[:8]: print(x)
    print("seed %d mode %s%s: %d problems" % (seed, ["USB", "Puck", "BT"][mode], " +disconnects" if disc else "", len(f)))
