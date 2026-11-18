"""Option 'Steam = Xbox Guide' (xinput_guide.c) and the remapper's Xbox Guide action."""
import ctypes
from simlib import *
s = Sim(); s.use(Profile()); s.run(5)
f = s.lib.xinput_guide_tap; f.argtypes = [ctypes.c_uint32, ctypes.c_int64]; f.restype = ctypes.c_bool
STEAM = 1 << 16
T = [1000]
def seq(steps):
    """steps: (buttons, ms) -> ms during which Guide was on (time keeps running, like k_uptime_get)"""
    t, on = T[0], 0
    for b, ms in steps:
        for _ in range(ms):
            t += 1; on += f(b, t)
    for _ in range(300): t += 1; on += f(0, t)
    T[0] = t
    return on
ok = fail = 0
def check(n, c, i=""):
    global ok, fail
    if c: ok += 1; print("PASS", n)
    else: fail += 1; print("FAIL", n, i)
check("option off: nothing", seq([(STEAM, 80)]) == 0)
s.cfg("xi_guide", 1)
v = seq([(STEAM, 80)]); check("lone tap of Steam = Guide 100 ms", v == 100, v)
check("Steam + D-pad (profile switch) = no Guide", seq([(STEAM, 30), (STEAM | 1 << 13, 40), (STEAM, 30)]) == 0)
check("Steam + Y (power) = no Guide", seq([(STEAM, 30), (STEAM | 1 << 3, 400)]) == 0)
check("touching the sticks / grips does not count as a chord", seq([(STEAM | 1 << 20 | 1 << 28, 80)]) == 100)
check("a long hold of Steam (menu / power) = no Guide", seq([(STEAM, 2000)]) == 0)
check("a 400 ms press still counts as a tap", seq([(STEAM, 400)]) == 100)
# the remapper's action Xbox Guide (0xA0 + 16) reaches the Xbox output
p = Profile(); p.bind("L4", XBOX + 16)
s2 = Sim(); s2.use(p); s2.run(5); s2.run(20, L4=1)
check("action 'Xbox Guide' sets the Steam bit of the Xbox buttons", s2.lib.sim_xbox_buttons() & STEAM, hex(s2.lib.sim_xbox_buttons()))
# "Xbox LT / RT (whole trigger)": analog on the soft pull, full on other sources
def xt(sim, t): return sim.lib.sim_xbox_trigger(t)
p = Profile(); p.bind("LT_SOFT", XTRIG_L); p.bind("RT_SOFT", XTRIG_L); p.bind("A", XTRIG_R)
s3 = Sim(); s3.use(p); s3.run(5)
check("trigger released: LT 0", xt(s3, 0) == 0, xt(s3, 0))
s3.run(10, tl=8000); v = xt(s3, 0); check("LT 25 %: analog ~62 (below the soft point too)", 55 <= v <= 70, v)
check("Steam gets no trigger (bound)", s3.out["tl"] == 0, s3.out["tl"])
s3.run(10, tl=32767); check("LT full: 255", xt(s3, 0) == 255, xt(s3, 0))
s3.run(10, tl=0, tr=16000); v = xt(s3, 0); check("right trigger bound to Xbox LT drives LT", 120 <= v <= 130, v)
s3.run(10, tr=0); check("both released: 0", xt(s3, 0) == 0 and xt(s3, 1) == 0)
s3.run(20, A=1); check("A bound to Xbox RT: full", xt(s3, 1) == 255, xt(s3, 1))
s3.run(20, A=0); check("A released: RT 0", xt(s3, 1) == 0, xt(s3, 1))
p2 = Profile(); p2.bind("LT_SOFT", XTRIG_L); p2.flags(PF_BLOCK); p2.u16(110, 2000); p2.u16(112, 8000)
s4 = Sim(); s4.use(p2); s4.run(5)
s4.run(10, tl=int(32767 * 0.5)); v = xt(s4, 0); check("analog start/end 20..80 %: 50 % -> ~127, unbound inputs blocked", 120 <= v <= 135, v)
s4.run(10, tl=int(32767 * 0.15)); check("below the start: 0", xt(s4, 0) == 0, xt(s4, 0))
s4.run(10, tl=int(32767 * 0.9)); check("past the end: 255", xt(s4, 0) == 255, xt(s4, 0))
print("\n%d passed, %d failed" % (ok, fail))
