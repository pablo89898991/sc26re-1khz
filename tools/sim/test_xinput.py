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
print("\n%d passed, %d failed" % (ok, fail))
