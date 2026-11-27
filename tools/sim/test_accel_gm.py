"""Accelerometer with the gyro mouse / joystick (real gyro_mouse.c), gestures and tilt -> stick:
1 always works (flags 0); 2 off while aiming (0x80: activation held; "always" activation = all
the time it is on); 3 as 2, but works with "always" activation (0x80|0x40)."""
import os
os.environ.setdefault("SIMLIB", "libsim_gm.so")
import struct
from simlib import *

ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)

G = 8192
T = 1 << BTN["RS_TOUCH"]
def blob(flags, stick=0):
    gest = [0] * 14; gest[2 * 1] = key("s")   # pull back -> s
    return struct.pack("<BBHHBBHH", stick, flags, 100 if stick else 0, 0, 0, 0, 0, 0) + bytes(gest) + bytes(6)
def setup(flags, act_mode=1, act_btn="RS_TOUCH", gm=1, stick=0):
    s = Sim(); s.use(Profile())
    b = blob(flags, stick)
    for o in range(0, 32, 16): assert s.feature(0xEE, "sc26re/acc/0/%x" % o, b[o:o + 16])[0] == 0
    assert s.feature(0xEF, "sc26re/acc/0")[0] == 0
    s.cfg("gm_act_button", BTN[act_btn] if act_btn else -1); s.cfg("gm_act_mode", act_mode); s.cfg("gm_enabled", gm)
    s.run(600, ax=0, ay=0, az=G); return s
def pull(s, buttons=0):
    base = dict(ax=0, ay=0, az=G, buttons=buttons)   # run() keeps fields: buttons always given
    s.run(400, **base)   # a jolt within 0.7 s of the last one is shaking: space them out
    k = s.run(30, **dict(base, ay=-2 * G)).kbd() + s.run(30, **dict(base, ay=2 * G)).kbd() + s.run(500, **base).kbd()
    return any(key("s") in st for st in k)
def tilt(s, buttons=0):
    # 30 degrees to the side: the left stick at 2/3
    s.run(300, ax=int(G * 0.5), ay=0, az=int(G * 0.866), buttons=buttons)
    x = s.out["lsx"]; s.run(300, ax=0, ay=0, az=G, buttons=buttons); return abs(x) > 8000

for name, fl in (("1 always", 0), ("2 off while aiming", 0x80), ("3 off while aiming, works with always", 0xC0)):
    paused_aim = fl != 0
    s = setup(fl)
    check(f"{name}: stick touch activation, thumb off: gesture", pull(s))
    check(f"{name}: thumb on the stick: gesture {'off' if paused_aim else 'works'}", pull(s, T) != paused_aim)
    check(f"{name}: let go again: gesture", pull(s))
    s = setup(fl, act_mode=0)
    paused_always = fl == 0x80
    check(f"{name}: activation 'always': gesture {'off' if paused_always else 'works'}", pull(s) != paused_always)
    s = setup(fl, act_btn=None)
    check(f"{name}: no activation input (always): gesture {'off' if paused_always else 'works'}", pull(s) != paused_always)
    s = setup(fl, act_mode=0, gm=0)
    check(f"{name}: gyro mouse off: gesture", pull(s))
    s = setup(fl, act_mode=2)
    check(f"{name}: 'active when released', thumb off (aims): gesture {'off' if paused_aim else 'works'}", pull(s) != paused_aim)
    check(f"{name}: 'active when released', thumb on: gesture", pull(s, T))
    s = setup(fl, stick=1)
    check(f"{name}: tilt -> stick, thumb off", tilt(s))
    check(f"{name}: tilt -> stick, thumb on: {'off' if paused_aim else 'works'}", tilt(s, T) != paused_aim)
    s = setup(fl, act_mode=0, stick=1)
    check(f"{name}: tilt -> stick, activation 'always': {'off' if paused_always else 'works'}", tilt(s) != paused_always)
print("\n%d passed, %d failed" % (ok, fail))
