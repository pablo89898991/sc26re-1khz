"""Gyro mouse (the real gyro_mouse.c) together with the remapper."""
import os, ctypes
os.environ.setdefault("SIMLIB", "libsim_gm.so")
from simlib import *

def gsim(p, **cfg):
    s = Sim(); s.lib.sim_gyro.argtypes = [ctypes.c_float] * 3
    s.use(p)
    for k, v in cfg.items(): s.cfg(k, v)
    s.run(5); return s

mv = lambda e: sum(abs(m[1]) + abs(m[2]) for m in e.mouse())
ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)

for label, block, bind in (("normal", False, False), ("unbound do nothing", True, False), ("touch bound to a key", False, True)):
    p = Profile(); p.bind("A", key("a"))
    if block: p.flags(PF_BLOCK)
    if bind: p.bind("RS_TOUCH", key("t"))
    s = gsim(p, gm_enabled=1, gm_act_button=20, gm_act_mode=1)
    s.lib.sim_gyro(120, 120, 120)
    a, b = mv(s.run(300)), mv(s.run(300, RS_TOUCH=1))
    check("gyro activation by stick touch (%s)" % label, a == 0 and b > 1000, (a, b))
p = Profile(); p.bind("R5", key("r")); p.flags(PF_BLOCK)
s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_ratchet=8)
s.lib.sim_gyro(150, 150, 0)
a, b = mv(s.run(300)), mv(s.run(300, R5=1))
check("ratchet on a bound back button pauses the gyro", a > 1000 and b < a / 10, (a, b))
print("\n%d passed, %d failed" % (ok, fail))
