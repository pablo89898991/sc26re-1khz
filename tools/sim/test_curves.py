"""Custom response curves (v51.7): sc26re/crv/<slot>, sticks and the gyro (real gyro_mouse.c)."""
import os, ctypes, struct
os.environ.setdefault("SIMLIB", "libsim_gm.so")
from simlib import *

LS, RS, GMOUSE, GJOY = 0, 1, 2, 3
HALF = [8192, 4096, 16384, 8192, 24576, 12288, 32767, 16384]          # y = x / 2
SQUARE = [8192, 2048, 16384, 8192, 24576, 18432, 29491, 26542]         # about y = x^2

def crv_blob(on=0, mouse_max=0, curves=None):
    pts = [[0] * 8 for _ in range(4)]
    for k, v in (curves or {}).items(): pts[k] = v
    return struct.pack("<BBh", on, 0, mouse_max) + b"".join(struct.pack("<8H", *c) for c in pts)

def put_curves(s, slot, blob, chunk=40):
    for o in range(0, len(blob), chunk):
        assert s.feature(0xEE, "sc26re/crv/%s/%x" % (slot, o), blob[o:o + chunk])[0] == 0
    assert s.feature(0xEF, "sc26re/crv/%s" % slot)[0] == 0

def get_curves(s, slot):
    out = b""
    while len(out) < 68:
        d = s.read("sc26re/crv/%s/%x" % (slot, len(out)))
        assert d, len(out)
        out += d
    return out[:68]

ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)

def gsim(p, **cfg):
    s = Sim(); s.lib.sim_gyro.argtypes = [ctypes.c_float] * 3
    s.use(p)
    for k, v in cfg.items(): s.cfg(k, v)
    s.run(5); return s

mv = lambda e: sum(abs(m[1]) + abs(m[2]) for m in e.mouse())
GM = dict(gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_curve=10, gm_accel=0)

# ---- storage: read back, survives a reboot, slots stay apart
s = Sim(); p = Profile(); p.bind("A", key("a")); s.use(p)
blob = crv_blob(on=1 << GMOUSE, mouse_max=500, curves={GMOUSE: HALF})
put_curves(s, "g", blob)
check("global curves read back", get_curves(s, "g") == blob, get_curves(s, "g").hex())
check("profile slot untouched", get_curves(s, 0) == bytes(68))
s.reboot()
check("curves survive a reboot", get_curves(s, "g") == blob)
check("bad slot is not a path", s.read("sc26re/crv/7/0") is None)
# a stage that is never committed changes nothing
s.feature(0xEE, "sc26re/crv/1/0", b"\x0f")
check("staged only: not applied", get_curves(s, 1)[0] == 0)

# ---- gyro mouse: global curve y = x/2 halves the movement, 1:1 diagonal keeps it
p = Profile(); p.bind("A", key("a"))
s = gsim(p, **GM); s.lib.sim_gyro(120, 120, 0); base = mv(s.run(300))
s = gsim(p, **GM); put_curves(s, "g", crv_blob(1 << GMOUSE, 0, {GMOUSE: HALF})); s.lib.sim_gyro(120, 120, 0)
half = mv(s.run(300))
check("gyro mouse: global curve y = x/2", base > 1000 and abs(half / base - 0.5) < 0.06, (base, half))
diag = [6553, 6553, 13107, 13107, 19660, 19660, 26214, 26214]
s = gsim(p, **GM); put_curves(s, "g", crv_blob(1 << GMOUSE, 0, {GMOUSE: diag})); s.lib.sim_gyro(120, 120, 0)
same = mv(s.run(300))
check("gyro mouse: diagonal curve is 1:1", abs(same / base - 1) < 0.03, (base, same))
# above mouse_max the last segment goes on: at 2x the range still half
s = gsim(p, **GM); put_curves(s, "g", crv_blob(1 << GMOUSE, 100, {GMOUSE: HALF})); s.lib.sim_gyro(150, 150, 0)
fast = mv(s.run(300)); s = gsim(p, **GM); s.lib.sim_gyro(150, 150, 0); fbase = mv(s.run(300))
check("gyro mouse: past mouse_max the curve carries on", abs(fast / fbase - 0.5) < 0.06, (fbase, fast))
# a curve stored but switched off does nothing
s = gsim(p, **GM); put_curves(s, "g", crv_blob(0, 0, {GMOUSE: HALF})); s.lib.sim_gyro(120, 120, 0)
check("gyro mouse: curve off = usual response", abs(mv(s.run(300)) / base - 1) < 0.03)
# a profile with its own gyro settings ignores the global curve, and uses its own
pg = Profile(); pg.bind("A", key("a")); pg.flags(PF_GYRO)
s = gsim(pg, **GM); s.cfg("remap_on", 0)
# copy the global gyro settings into the profile the way the page does: read them back
vals = [s.cfg_get(k) for k in ("gm_output", "gm_act_button", "gm_act_mode", "gm_axis_pitch", "gm_axis_yaw", "gm_axis_roll",
                               "gm_sens", "gm_scale_p", "gm_scale_y", "gm_scale_r", "gm_curve", "gm_accel", "gm_accel_lo",
                               "gm_accel_hi", "gj_dz", "gj_anti", "gj_max", "gj_curve", "gj_tilt", "gm_haptic",
                               "gm_qt1_btn", "gm_qt1_deg", "gm_qt2_btn", "gm_qt2_deg", "gm_qt3_btn", "gm_qt3_deg",
                               "gm_qt4_btn", "gm_qt4_deg", "gm_qt_ms", "gm_ratchet")]
GYRO_OFF = 44   # remap_profile.gyro[]
for i, v in enumerate(vals): pg.s16(GYRO_OFF + 2 * i, v)
s = gsim(pg, **GM); put_curves(s, "g", crv_blob(1 << GMOUSE, 0, {GMOUSE: HALF})); s.lib.sim_gyro(120, 120, 0)
own = mv(s.run(300))
check("own gyro settings: the global curve is not used", abs(own / base - 1) < 0.05, (base, own))
put_curves(s, 0, crv_blob(1 << GMOUSE, 0, {GMOUSE: HALF})); s.run(5)
own2 = mv(s.run(300))
check("own gyro settings: the profile's curve is used", abs(own2 / base - 0.5) < 0.06, (base, own2))
# remapper off: the global curve again
s.cfg("remap_on", 0); s.run(5); off = mv(s.run(300))
check("remapper off: global curve", abs(off / base - 0.5) < 0.06, (base, off))

# ---- sticks: to Steam, through the curve (after the dead zones)
p = Profile(); p.analog(0, PASS)
s = Sim(); s.use(p); put_curves(s, 0, crv_blob(1 << LS, 0, {LS: HALF}))
s.run(3, lsx=16384); a = s.out["lsx"]
s.run(3, lsx=32767); b = s.out["lsx"]
check("left stick: curve y = x/2 at half", 7800 <= a <= 8600, a)
check("left stick: the curve ends at its last point", 16000 <= b <= 16800, b)
s.run(3, lsx=0); check("left stick: rest stays 0", s.out["lsx"] == 0)
check("right stick: not curved", (s.run(3, rsx=16384), s.out["rsx"])[1] == 16384 or s.out["rsx"] > 16000, s.out["rsx"])
# with an inner dead zone the curve starts at its edge
p = Profile(); p.analog(0, PASS, dz_in=1000)
s = Sim(); s.use(p); put_curves(s, 0, crv_blob(1 << LS, 0, {LS: SQUARE}))
s.run(3, lsx=3000); z = s.out["lsx"]
s.run(3, lsx=int(32767 * (0.1 + 0.9 * 0.5))); m = s.out["lsx"]
check("left stick: dead zone first, then the curve", z == 0 and 7800 <= m <= 8600, (z, m))
# another profile without curves: unchanged
put_curves(s, 1, crv_blob(0)); p2 = Profile(); p2.analog(0, PASS); s.upload(1, p2); s.cfg("remap_profile", 1)
s.run(3, lsx=16384); check("profile without a curve: linear", s.out["lsx"] == 16384, s.out["lsx"])
# stick as mouse: speed follows the curve instead of the squared response
p = Profile(); p.analog(1, MOUSE, speed=2000)
s = Sim(); s.use(p); e1 = s.run(300, rsx=16384); m1 = sum(m[1] for m in e1.mouse())
s = Sim(); s.use(p); put_curves(s, 0, crv_blob(1 << RS, 0, {RS: diag})); e2 = s.run(300, rsx=16384)
m2 = sum(m[1] for m in e2.mouse())
check("stick mouse: linear curve gives about twice the squared response at half", 1.6 < m2 / max(m1, 1) < 2.4, (m1, m2))

print("\n%d passed, %d failed" % (ok, fail))
raise SystemExit(1 if fail else 0)
