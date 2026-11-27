"""Slow return to centre ("Direct Lean", v51.9): lean.h against the filter it is made after
(filter_axis() of u/z50_Jumper's UNC THUMBS, damping off), and the stick output in the firmware."""
import os, ctypes, random, subprocess, tempfile, math
from simlib import *

ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)

# --- the reference: UNC THUMBS v2.1 filter_axis() with damping_enabled=False
def clamp(x, lo=-1.0, hi=1.0): return max(lo, min(hi, x))
def ref(physical, previous_physical, virtual, dt, return_seconds, active_multiplier):
    physical = clamp(physical); previous_physical = clamp(previous_physical); virtual = clamp(virtual); dt = max(0.0, dt)
    if return_seconds <= 0: return physical
    outward_right = max(0.0, physical) - max(0.0, previous_physical)
    outward_left = min(0.0, physical) - min(0.0, previous_physical)
    displacement = max(0.0, outward_right) + min(0.0, outward_left)
    virtual = clamp(virtual + displacement)
    step = dt / return_seconds; active_step = step * max(1.0, active_multiplier)
    if physical > 0 and virtual >= 0 and physical > virtual: virtual = min(physical, virtual + active_step)
    elif physical < 0 and virtual <= 0 and physical < virtual: virtual = max(physical, virtual - active_step)
    else:
        boundary = 0.0
        if virtual > 0 and physical > 0: boundary = min(virtual, physical)
        elif virtual < 0 and physical < 0: boundary = max(virtual, physical)
        inward_step = active_step if physical * virtual < 0 else step
        if virtual > boundary: virtual = max(boundary, virtual - inward_step)
        elif virtual < boundary: virtual = min(boundary, virtual + inward_step)
    return clamp(virtual)

# --- lean.h on its own
src = os.path.join(tempfile.gettempdir(), "lean_t.c"); so = src[:-2] + ".so"
open(src, "w").write('#include <math.h>\n#include "%s"\nfloat lf(float p,float pp,float v,float dt,float r,float m){return lean_filter(p,pp,v,dt,r,m);}\n'
                     % os.path.join(HERE, "../../app/src/lean.h"))
subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-o", so, src, "-lm"])
L = ctypes.CDLL(so); L.lf.restype = ctypes.c_float; L.lf.argtypes = [ctypes.c_float] * 6
rnd = random.Random(1); worst = 0.0; n = 0
for seq in range(400):
    r = rnd.choice([0.05, 0.5, 1.5, 2.0, 5.0]); m = rnd.choice([1.0, 1.0, 3.0, 8.0]); dt = rnd.choice([0.001, 0.008])
    p = pp = v = vr = 0.0
    for t in range(600):
        if rnd.random() < 0.08: p = rnd.choice([0.0, 0.0, 1.0, -1.0, rnd.uniform(-1, 1)])
        elif rnd.random() < 0.3: p = clamp(p + rnd.uniform(-0.05, 0.05))
        v = L.lf(p, pp, v, dt, r, m); vr = ref(p, pp, vr, dt, r, m); pp = p
        worst = max(worst, abs(v - vr)); n += 1
        vr = v   # follow the C value, so float32 rounding does not add up over a sequence
check("lean.h = UNC THUMBS filter_axis (damping off), %d random steps" % n, worst < 1e-5, worst)
check("off (0 s) = the stick", L.lf(0.3, 0.9, 0.9, 0.01, 0.0, 1.0) == ctypes.c_float(0.3).value)

# --- in the firmware: left stick to Steam, X only, 2 s back from full
def prof(ret=40, mult=20, axes=1, side=0):
    p = Profile(); p.b[3224 + 3 * side:3227 + 3 * side] = bytes([ret, mult, axes]); return p
def lsx(s): return s.out["lsx"] / 32767
s = Sim(); s.use(prof()); s.run(20)
s.state.update(lsx=32767, lsy=32767); s.run(50)
check("push out: 1:1 at once", lsx(s) > 0.999 and s.out["lsy"] == 32767, s.out)
s.state.update(lsx=0, lsy=0); s.run(1000)
check("let go: half way back after 1 s (2 s from full)", abs(lsx(s) - 0.5) < 0.01 and s.out["lsy"] == 0, (lsx(s), s.out["lsy"]))
s.run(1050); check("... and centred after 2 s", s.out["lsx"] == 0, lsx(s))
s.state.update(lsx=32767); s.run(50); s.state.update(lsx=16384); s.run(2000)
check("held half way: comes back only to the stick", abs(lsx(s) - 0.5) < 0.002, lsx(s))
# countersteer at 3x: from full right to half left in one go
s = Sim(); s.use(prof(mult=60)); s.run(20); s.state.update(lsx=32767); s.run(50)
s.state.update(lsx=-16384); s.run(1)
check("countersteer: moves by the stick's travel past the centre at once", abs(lsx(s) - 0.5) < 0.01, lsx(s))
s.run(330); check("... then to the centre 3x as fast (0.33 s)", abs(lsx(s)) < 0.01, lsx(s))
s.run(340); check("... and out to the stick 3x as fast", abs(lsx(s) + 0.5) < 0.005, lsx(s))
# right stick, both axes; Y alone; off
s = Sim(); s.use(prof(axes=3, side=1)); s.run(20); s.state.update(rsx=32767, rsy=-32767, lsx=32767); s.run(50); s.state.update(rsx=0, rsy=0, lsx=0); s.run(500)
check("right stick, X and Y", abs(s.out["rsx"] / 32767 - 0.75) < 0.01 and abs(s.out["rsy"] / 32767 + 0.75) < 0.01 and s.out["lsx"] == 0, s.out)
s = Sim(); s.use(prof(ret=0)); s.run(20); s.state.update(lsx=32767); s.run(50); s.state.update(lsx=0); s.run(10)
check("0 s: off", s.out["lsx"] == 0)
# a profile change starts it again from the stick (no lean carried into another profile)
s = Sim(); s.upload(1, Profile()); s.use(prof()); s.run(20); s.state.update(lsx=32767); s.run(50); s.state.update(lsx=0)
s.run(100); s.cfg("remap_profile", 1); s.run(20)
check("profile without it: the stick at once", s.out["lsx"] == 0, lsx(s))
s.cfg("remap_profile", 0); s.run(20)
check("back: starts from the stick, not the old lean", s.out["lsx"] == 0, lsx(s))
# the Xbox stick mode gets it too; a mode without it (mouse) starts again from the stick
p = prof(); p.analog(0, 12); s = Sim(); s.lib.sim_xbox(1); s.use(p); s.run(20)
s.state.update(lsx=32767); s.run(50); s.state.update(lsx=0); s.run(500)
check("Xbox stick: 3/4 back after 0.5 s", abs(s.lib.sim_xbox_stick(0, 0) / 32767 - 0.75) < 0.01, s.lib.sim_xbox_stick(0, 0))
print("\n%d passed, %d failed" % (ok, fail))
