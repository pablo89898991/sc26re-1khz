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
# held flat (gravity along the yaw axis): every space turns the same way as local space
dxs = {}
for space in (0, 1, 2):
    p = Profile(); p.bind("A", key("a"))
    s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_space=space)
    s.set(ax=0, ay=0, az=16000); s.run(50)
    s.lib.sim_gyro(0, 100, 0)
    e = s.run(300); dxs[space] = sum(m[1] for m in e.mouse())
check("flat: player / world space turn like local space", dxs[0] != 0 and all((dxs[k] > 0) == (dxs[0] > 0) and abs(dxs[k] - dxs[0]) < abs(dxs[0]) * 0.1 for k in (1, 2)), dxs)
# tilted 45 degrees forward: world space keeps the full yaw, local space loses part of it
dxs = {}
for space in (0, 2):
    p = Profile(); p.bind("A", key("a"))
    s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_space=space, gm_axis_roll=0)
    s.set(ax=0, ay=-11314, az=11314); s.run(50)            # gravity between yaw and roll
    s.lib.sim_gyro(0, 70.7, 70.7)                           # a turn about the world vertical
    e = s.run(300); dxs[space] = sum(m[1] for m in e.mouse())
check("tilted: a turn about the vertical, world space = full turn", abs(dxs[2]) > abs(dxs[0]) * 1.3, dxs)
# profile override of the space (ext.gyx.space = 3 = world)
p = Profile(); p.flags(PF_GYRO); p.gyx(space=3)
g = [0, -1, 0, 2, 1, 0, 100, 100, 100, 100, 10, 0, 0, 0, 0, 0, 0, 100, 30, 0, -1, 0, -1, 0, -1, 0, -1, 0, 60, -1]
for k, v in enumerate(g): p.s16(44 + 2 * k, v)
s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_axis_roll=0, gm_space=0)
s.set(ax=0, ay=16000, az=0); s.run(50); s.lib.sim_gyro(0, 0, -100)
e = s.run(300); dx = sum(m[1] for m in e.mouse())
check("profile gyro space (world) overrides the global setting", abs(dx) > 300, dx)
# fuzz gm 9423: a profile's own gyro values outside the ranges (a negative tilt limit) held the
# gyro joystick in a corner with the controller at rest; they count as the page would store them
GK = ["gm_output", "gm_act_button", "gm_act_mode", "gm_axis_pitch", "gm_axis_yaw", "gm_axis_roll", "gm_sens", "gm_scale_p", "gm_scale_y", "gm_scale_r",
      "gm_curve", "gm_accel", "gm_accel_lo", "gm_accel_hi", "gj_dz", "gj_anti", "gj_max", "gj_curve", "gj_tilt", "gm_haptic"]
for label, tilt in (("negative", -5), ("zero", 0), ("huge", 30000)):
    p = Profile(); p.flags(PF_GYRO | PF_GYRO_ON)
    vals = dict(gm_output=3, gm_act_button=-1, gm_act_mode=0, gm_axis_pitch=2, gm_axis_yaw=1, gm_axis_roll=0, gm_sens=100, gm_scale_p=100, gm_scale_y=100,
                gm_scale_r=100, gm_curve=10, gj_dz=0, gj_anti=0, gj_max=300, gj_curve=100, gj_tilt=tilt)
    for k, n in enumerate(GK): p.s16(44 + 2 * k, vals.get(n, 0))
    for k in range(20, 30): p.s16(44 + 2 * k, -1 if k % 2 == 0 and k < 28 else 0)
    s = gsim(p, gm_enabled=1)
    s.set(ax=0, ay=0, az=16000); s.run(300)
    rest = (s.out["rsx"], s.out["rsy"])
    s.lib.sim_gyro(0, 40, 0); s.run(200); s.lib.sim_gyro(0, 0, 0); turned = (s.out["rsx"], s.out["rsy"])
    check("gyro joystick deflection, %s tilt limit in the profile: centred at rest, moves when turned" % label, rest == (0, 0) and turned != (0, 0), (rest, turned))
# dead bands (hard / soft), "hold to turn off", the 360 calibration with its button, precision smoothing
p = Profile(); p.bind("B", key("b"))
for soft in (0, 1):
    s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_band_y_lo=-500, gm_band_y_hi=500, gm_band_soft=soft, gm_axis_roll=0)
    s.lib.sim_gyro(0, 3, 0); slow = mv(s.run(300))
    s.lib.sim_gyro(0, 20, 0); fast = mv(s.run(300))
    check("gyro mouse dead band (%s): 3 deg/s inside does nothing, 20 deg/s moves" % ("soft" if soft else "hard"), slow == 0 and fast > 50, (slow, fast))
    if soft: soft_fast = fast
    else: hard_fast = fast
check("soft dead band starts from 0 at its edge (less than hard)", soft_fast < hard_fast * 0.85, (soft_fast, hard_fast))
s = gsim(p, gm_enabled=1, gm_act_button=17, gm_act_mode=2)
s.lib.sim_gyro(0, 60, 0)
a, b = mv(s.run(300)), mv(s.run(300, L4=1))
check("gyro mouse 'hold to turn off'", a > 100 and b < 10, (a, b))
s = gsim(p, gm_enabled=1, gm_cal_button=1, gm_cal_speed=1500)
s.lib.gyro_mouse_cal_start(); s.run(20)
n0 = len(s.log)
s.run(30, B=1); hidden = not s.out_btn("B"); s.run(30, B=0)
s.run(2000)
s.run(30, B=1); s.run(30, B=0); s.run(50)
keyed = any(key("b") in k for k in Events(s.log[n0:]).kbd()) if isinstance(s.log, list) else None
check("360 calibration: its button's own binding (a key in the game) does not fire (v51 test 85)", keyed is False, keyed)
c = s.cfg_get("gm_cal_hi") * 32768 + s.cfg_get("gm_cal_lo")
check("360 calibration with a bound button: it starts and stops the turn, the counts are saved", 2900 < c < 3300 and hidden, (c, hidden))
s.run(30, B=1); back = s.out_btn("B") or any(key("b") in k for k in Events(s.log).kbd()); s.run(30, B=0)
check("after the calibration the button is the game's again", back)
# Steam + A starts the 360 calibration, Steam + View toggles the gyro mouse: none of those buttons'
# bindings fire (v51 test 86; with LB + RB + View (+ A) LB and RB went to the game first)
p3 = Profile(); p3.bind("LB", key("f")); p3.bind("RB", key("g")); p3.bind("VIEW", key("tab")); p3.bind("A", key("b"))
s = gsim(p3, gm_enabled=0)
n0 = len(s.log)
s.run(30, STEAM=1); s.run(30, STEAM=1, VIEW=1); s.run(30, STEAM=0, VIEW=1); s.run(30)
on = s.cfg_get("gm_enabled")
s.run(30, STEAM=1); s.run(30, STEAM=1, A=1); s.run(30, STEAM=0, A=1); s.run(30)
later = [k for k in Events(s.log[n0:]).kbd()]
check("Steam + View: gyro mouse on, Steam + A: no key of the game fires (v51 test 86)", on == 1 and not any(later), (on, later))
n1 = len(s.log); s.run(30, A=1); hid = not s.out_btn("A") and not any(Events(s.log[n1:]).kbd()); s.run(30, A=0)
check("Steam + A started the 360 calibration: its button (A) starts the turn, hidden from the game and the binding", hid)
# an unbound button, which the game sees as a button (with B bound above the game never sees B anyway)
s = gsim(Profile(), gm_enabled=1, gm_cal_button=17, gm_cal_speed=1500)
s.lib.gyro_mouse_cal_start(); s.run(20)
s.run(30, L4=1); st = s.out_btn("L4"); s.run(30, L4=0); s.run(2000)
s.run(30, L4=1); sp = s.out_btn("L4"); s.run(30, L4=0); s.run(50)
s.run(30, L4=1); after = s.out_btn("L4"); s.run(30, L4=0)
check("360 calibration on an unbound button: the start and the stop press stay hidden until released, then it is the game's", not st and not sp and after, (st, sp, after))
# quick turn and the gyro on/off chord with "unbound send nothing" (their buttons unbound): they still work
p2 = Profile(); p2.flags(PF_BLOCK)
s = gsim(p2, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_qt1_btn=17, gm_qt1_deg=90, gm_qt_ms=0)
qt = sum(m[1] for m in s.run(20, L4=1).mouse()); s.run(20, L4=0)
check("quick turn on an unbound button with 'unbound send nothing'", abs(qt) > 1000, qt)
s.run(30, STEAM=1); s.run(30, STEAM=1, VIEW=1); s.run(30, STEAM=0, VIEW=0); s.run(20)
check("Steam + View turns the gyro mouse off with 'unbound send nothing'", s.cfg_get("gm_enabled") == 0, s.cfg_get("gm_enabled"))
s = gsim(p, gm_enabled=1, gm_act_button=-1, gm_act_mode=0, gm_smooth=50, gm_axis_roll=0)
s.lib.sim_gyro(0, 2, 0); a = s.run(400).mouse(); s.lib.sim_gyro(0, 0, 0); s.run(400)
check("precision smoothing: a slow turn still moves, nothing left moving after it", sum(abs(m[1]) for m in a) > 0 and mv(s.run(300)) == 0)
print("\n%d passed, %d failed" % (ok, fail))
