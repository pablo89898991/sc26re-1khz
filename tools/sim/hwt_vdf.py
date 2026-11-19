"""Hardware test cases from real Steam Input layouts (.vdf): every action set imported the way the
remapper page does (web/vdf.js), then every bound input of it pressed in turn, plain and with
its mode shift held. The oracle: each input's own outputs (keys, mouse buttons, pad / Xbox
buttons) appear while it is pressed (or right after, for long / double / release presses).

The files: VDF_DIR (default /mnt/user-data/uploads), *_controller_config.vdf. Without them the
cases are left out (web/hwtests.json keeps what was exported)."""
import glob, os, struct
import hwt
from hwt import S, N, case, need, K
from simlib import BTN, SHIFT_SRC, Profile

DIR = os.environ.get("VDF_DIR", "/mnt/user-data/uploads")
B_OFF = lambda s: 488 + 14 * s
SB_OFF = lambda k: 1412 + 14 * k
AN_OFF = lambda i: 256 + 18 * i
SH_OFF = lambda g: 328 + 20 * g
BTN_NAME = {v: k for k, v in BTN.items()}
SHIFT_GROUP_OF = {}
for g, srcs in enumerate(((0, 1, 2, 3), (13, 10, 12, 11), (30, 31, 32, 33, 60), (34, 35, 36, 37, 61),
                          (38, 39, 40, 41, 46, 47, 48, 49, 54, 55, 62, 64), (42, 43, 44, 45, 50, 51, 52, 53, 56, 57, 63, 65),
                          (58, 27), (59, 23))):
    for s in srcs: SHIFT_GROUP_OF[s] = g


def expected(acts):
    """what an action set sends that the log shows: keys, mouse buttons, pad buttons"""
    keys, mb, btn = set(), 0, 0
    for a in acts:
        if 0x04 <= a <= 0x65 or 0xE0 <= a <= 0xE7: keys.add(a)
        elif 0x70 <= a <= 0x71: mb |= 1 << (a - 0x70)
        elif a == 0x7A: mb |= 4
        elif 0x7B <= a <= 0x7C: mb |= 8 << (a - 0x7B)
        elif 0x80 <= a < 0x80 + 30: btn |= 1 << (a - 0x80)
        elif 0xA0 <= a < 0xA0 + 30: btn |= 1 << (a - 0xA0)   # Xbox outputs reach the Steam pad without the Xbox output
    return keys, mb, btn


def binding(b, off):
    mode, acts, hap = b[off], [list(b[off + 1 + 4 * g:off + 5 + 4 * g]) for g in range(3)], b[off + 13]
    return mode, [[a for a in x if a] for x in acts], hap >> 2


def press_for(b, s, shifted):
    """the input frame that presses source s in profile b, or None (not pressable here)"""
    def an(i):
        o = AN_OFF(i) if not shifted else SH_OFF(2 + i) + 2
        return b[o], b[o + 1]
    if s < 30:
        st = {BTN_NAME[s]: 1}
        if s == 27: st["tl"] = 32767
        if s == 23: st["tr"] = 32767
        if s in (26, 22): st["LP_TOUCH" if s == 26 else "RP_TOUCH"] = 1
        return st
    if s in (58, 59):
        th = struct.unpack_from("<H", b, 18 + 2 * (s - 58))[0] or 5000
        v = min(31000, int((th + 2500) * 32767 / 10000))
        return {"tl" if s == 58 else "tr": v}
    if 30 <= s < 38:
        i = (s - 30) // 4; d = (s - 30) % 4
        mode, _ = an(i)
        if mode not in (1, 2): return None
        x, y = ((0, 1), (0, -1), (-1, 0), (1, 0))[d]
        return {("lsx" if i == 0 else "rsx"): x * 28000, ("lsy" if i == 0 else "rsy"): y * 28000}
    if 38 <= s < 46:
        side = (s - 38) // 4; d = (s - 38) % 4
        mode, flags = an(2 + side)
        if mode not in (1, 2, 9): return None
        x, y = ((0, 1), (0, -1), (-1, 0), (1, 0))[d]
        k, f = ("LP", "l") if side == 0 else ("RP", "r")
        st = {k + "_TOUCH": 1, f + "px": x * 28000, f + "py": y * 28000}
        if flags & 1: st[k + "_CLICK"] = 1
        if flags & 0x10: st[f + "pp"] = 32000
        return st
    return None


def cases_for(name, slot, profiles):
    b = bytes(profiles[slot])
    items = []  # (source, shifted shift-group or None, binding offset)
    for s in range(66):
        if b[B_OFF(s)] or any(b[B_OFF(s) + 1:B_OFF(s) + 13]): items.append((s, None, B_OFF(s)))
    for k, s in enumerate(SHIFT_SRC):
        o = SB_OFF(k)
        if any(b[o + 1:o + 13]): items.append((s, SHIFT_GROUP_OF.get(s), o))
    plan = []
    for s, g, off in items:
        mode, acts, chord = binding(b, off)
        if chord or not acts[0] and not acts[2]: continue          # chorded: tested by hand cases
        if any(a >= 0xC0 for grp in acts for a in grp): continue  # profile / layer / macro / stick / trigger actions
        shb = b[SH_OFF(g)] if g is not None else 0
        if g is not None and not (1 <= shb <= 30): continue
        st = press_for(b, s, g is not None)
        if st is None: continue
        if g is not None: st = dict(st, **{BTN_NAME[shb - 1]: 1})
        exp = expected(acts[2] if (mode & 0x10) and not acts[0] else acts[0])
        if not any(exp): continue
        plan.append((s, g, mode, st, exp, shb))
    out = []
    for n in range(0, len(plan), 9):
        chunk = plan[n:n + 9]
        steps, wins = [N(80)], []
        t = 80
        for s, g, mode, st, exp, shb in chunk:
            if g is not None:
                steps.append(S(60, **{BTN_NAME[shb - 1]: 1})); t += 60
            hold = 120
            steps.append(S(hold, **st))
            gap = 350 if mode & 0x02 else 220
            steps.append(N(gap) if g is None else N(gap))
            wins.append((s, g, t, t + hold + gap - 20, exp))
            t += hold + gap
        steps.append(N(100))

        def oracle(l, P, wins=wins):
            for s, g, t0, t1, (keys, mb, btn) in wins:
                win = [e for e in l if t0 <= e["t"] <= t1]
                nm = (BTN_NAME.get(s, str(s))) + (" (shifted)" if g is not None else "")
                if keys: need(any(keys <= set(e["keys"]) for e in win), "%s -> keys %s" % (nm, "+".join(hwt.KN.get(k, "%02x" % k) for k in sorted(keys))), P)
                if mb: need(any(e["mb"] & mb == mb for e in win), "%s -> mouse %x" % (nm, mb), P)
                if btn: need(any(e["btn"] & btn == btn for e in win), "%s -> pad %x" % (nm, btn), P)
        title = "%s, set %d, inputs %d-%d of %d" % (name, slot + 1, n + 1, n + len(chunk), len(plan))
        out.append((title, steps, oracle))
    return out


def gestures():
    """every analog input moved: sticks round, pads touched / slid / circled / clicked, triggers
    pulled, the controller tilted and turned. The oracle: nothing stays held at the end; the rest
    is the comparison with the simulator"""
    import math
    st = [N(60)]
    for side in (0, 1):
        for d in (90, 0, 270, 180):
            a = math.radians(d); k = ("lsx", "lsy") if side == 0 else ("rsx", "rsy")
            st.append(S(70, **{k[0]: int(30000 * math.cos(a)), k[1]: int(30000 * math.sin(a))}))
        st.append(N(80))
    for side, (k, f) in enumerate((("LP", "l"), ("RP", "r"))):
        st += [S(8, **{k + "_TOUCH": 1, f + "px": -15000 + i * 5000, f + "py": 0}) for i in range(4)]
        st += [S(60, **{k + "_TOUCH": 1, k + "_CLICK": 1, f + "px": 20000, f + "py": 20000}), N(80)]
    st += [S(120, tl=20000), S(120, tl=32767, LT_CLICK=1), N(80), S(120, tr=32767, RT_CLICK=1), N(80)]
    st += [S(200, az=14000), S(200, az=14000, gz=-400), S(150, az=14000), N(120)]
    def oracle(l, P):
        e = l[-1]
        need(not e["keys"] and not e["mb"], "nothing held at the end (keys %s, mouse %x)" % (e["keys"], e["mb"]), P)
    return st, oracle


def register():
    files = sorted(glob.glob(os.path.join(DIR, "*_controller_config.vdf")))
    seen = set()
    for path in files:
        wid = os.path.basename(path).split("-")[-1].split("_")[0]
        if wid in seen: continue
        seen.add(wid)
        from test_vdf import vdf
        r = vdf(open(path, encoding="utf-8").read(), {"padXbox": True})
        profs = r["profiles"]
        for slot, pb in enumerate(profs):
            if pb is None: continue
            extra = {k: bytes(p) for k, p in enumerate(profs) if p is not None and k != slot}
            gs, go = gestures()
            for i, (title, steps, oracle) in enumerate(cases_for(wid, slot, profs) + [("%s, set %d, sticks / pads / triggers / gyro moved" % (wid, slot + 1), gs, go)]):
                def f(pb=pb, steps=steps, oracle=oracle, extra=extra):
                    p = Profile(); p.b[:] = bytes(pb)
                    for k, v in extra.items():
                        q = Profile(); q.b[:] = v; hwt._extra[k] = q
                    return p, steps, oracle
                gest = "moved" in title
                case("vdf_%s_s%d_%d" % (wid, slot + 1, i + 1), "Steam layout " + title, slot=slot, snap=gest, rel=0.2 if gest else 0.06)(f)
