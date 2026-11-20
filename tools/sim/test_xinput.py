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
# "Xbox stick" mode: sticks / trackpads to the Xbox pad's sticks
def xs(sim, i): return (sim.lib.sim_xbox_stick(i, 0), sim.lib.sim_xbox_stick(i, 1))
p = Profile(); p.analog(0, XSTICK); p.analog(1, XSTICK, flags=AF_OTHER)
s5 = Sim(); s5.use(p); s5.run(5)
s5.run(10, lsx=20000, lsy=-10000); v = xs(s5, 0)
check("left stick -> Xbox left stick", abs(v[0] - 20000) < 400 and abs(v[1] + 10000) < 400, v)
check("Steam gets no left stick", s5.out["lsx"] == 0 and s5.out["lsy"] == 0, (s5.out["lsx"], s5.out["lsy"]))
s5.run(10, lsx=0, lsy=0, rsx=-32767); v = xs(s5, 0)
check("right stick (other side) -> Xbox left stick", v[0] < -32000, v)
s5.run(10, rsx=0); check("released: centred", xs(s5, 0) == (0, 0) and xs(s5, 1) == (0, 0), (xs(s5, 0), xs(s5, 1)))
p = Profile(); p.analog(3, XSTICK)
s6 = Sim(); s6.use(p); s6.run(5)
s6.run(10, buttons=1 << 21, rpx=16000, rpy=16000); v = xs(s6, 1)
check("right trackpad (touched) -> Xbox right stick", v[0] > 5000 and v[1] > 5000, v)
s6.run(10, buttons=0, rpx=0, rpy=0); check("finger lifted: centred", xs(s6, 1) == (0, 0), xs(s6, 1))

# the Steam button to Steam (steam_button_hold_back): the controller's Steam chords never reach Steam
h = s.lib.steam_button_hold_back; h.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int64]; h.restype = ctypes.c_uint32
def hseq(steps, out_steam=True):
    """steps: (physical buttons, ms) -> (ms Steam reached Steam, first ms it did)"""
    t, on, first = T[0], 0, None
    for b, ms in steps + [(0, 300)]:
        for _ in range(ms):
            t += 1; o = h(b if out_steam else b & ~STEAM, b, t)
            if o & STEAM: on += 1; first = first if first is not None else t - T[0]
    T[0] = t
    return on, first
v = hseq([(STEAM, 80)]); check("Steam tap: a 100 ms tap goes to Steam after it is let go", v == (100, 81), v)
v = hseq([(STEAM, 30), (STEAM | 1 << 13, 40), (STEAM, 30)]); check("Steam + D-pad (profile): Steam never sees Steam", v[0] == 0, v)
v = hseq([(STEAM, 200), (STEAM | 1 << 3, 600)]); check("Steam + Y (power off): Steam never sees Steam", v[0] == 0, v)
v = hseq([(STEAM, 50), (STEAM | 1 << 2, 100), (STEAM, 50)]); check("Steam + X (Steam's keyboard): Steam gets it as X joins", v == (150, 51), v)
v = hseq([(STEAM | 1 << 23, 100)]); check("RT held before Steam (screenshot chord): Steam at once", v == (100, 1), v)
v = hseq([(STEAM, 1500)]); check("long hold: Steam gets it after 0.6 s, no tap afterwards", v == (900, 601), v)
v = hseq([(STEAM | 1 << 20 | 1 << 28 | 1 << 24, 80)]); check("touches / grips are no chord", v == (100, 81), v)
v = hseq([(STEAM, 700), (STEAM | 1 << 13, 40)]); check("D-pad after the 0.6 s: Steam already has it (known limit)", v == (140, 601), v)
v = hseq([(STEAM, 80)], out_steam=False); check("remapper hides Steam: nothing", v[0] == 0, v)
print("\n%d passed, %d failed" % (ok, fail))
