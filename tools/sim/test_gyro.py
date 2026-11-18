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
# gyro spaces: controller held upright (triggers up), turned about the vertical
for space, want in ((0, False), (1, True), (2, True)):
    p = Profile(); p.bind("A", key("a"))
    s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_axis_roll=0, gm_space=space)
    s.set(ax=0, ay=16000, az=0); s.run(50)
    s.lib.sim_gyro(0, 0, -100)                # about the controller's Y = the world vertical
    e = s.run(300); dx = sum(m[1] for m in e.mouse()); dy = sum(m[2] for m in e.mouse())
    check("space %d: turn about gravity moves x=%s" % (space, want), (abs(dx) > 300) == want and abs(dy) < 50, (dx, dy))
# profile override of the space (ext.gyx.space = 3 = world)
p = Profile(); p.flags(PF_GYRO); p.gyx(space=3)
g = [0, -1, 0, 2, 1, 0, 100, 100, 100, 100, 10, 0, 0, 0, 0, 0, 0, 100, 30, 0, -1, 0, -1, 0, -1, 0, -1, 0, 60, -1]
for k, v in enumerate(g): p.s16(44 + 2 * k, v)
s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_axis_roll=0, gm_space=0)
s.set(ax=0, ay=16000, az=0); s.run(50); s.lib.sim_gyro(0, 0, -100)
e = s.run(300); dx = sum(m[1] for m in e.mouse())
check("profile gyro space (world) overrides the global setting", abs(dx) > 300, dx)
print("\n%d passed, %d failed" % (ok, fail))
