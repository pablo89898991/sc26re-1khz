"""Accelerometer "off while the gyro mouse / joystick is on" with the real gyro_mouse.c: off only while the
gyro moves the cursor (its activation held), not all the time it is switched on (v51.8: always off)."""
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
def blob(flags):
    gest = [0] * 14; gest[2 * 1] = key("s")   # pull back -> s
    return struct.pack("<BBHHBBHH", 0, flags, 0, 0, 0, 0, 0, 0) + bytes(gest) + bytes(6)
def setup(flags, act_mode=1):
    s = Sim(); s.use(Profile())
    b = blob(flags)
    for o in range(0, 32, 16): assert s.feature(0xEE, "sc26re/acc/0/%x" % o, b[o:o + 16])[0] == 0
    assert s.feature(0xEF, "sc26re/acc/0")[0] == 0
    s.cfg("gm_act_button", BTN["RS_TOUCH"]); s.cfg("gm_act_mode", act_mode); s.cfg("gm_enabled", 1)
    s.run(600, ax=0, ay=0, az=G); return s
def pull(s, buttons=0):
    base = dict(ax=0, ay=0, az=G, buttons=buttons)   # run() keeps fields: buttons always given
    k = s.run(30, **dict(base, ay=-2 * G)).kbd() + s.run(30, **dict(base, ay=2 * G)).kbd() + s.run(500, **base).kbd()
    return any(key("s") in st for st in k)
s = setup(0x80)
check("gyro mouse on, not touching the stick: the gesture works", pull(s))
s.run(100, ax=0, ay=0, az=G, buttons=1 << BTN["RS_TOUCH"])   # aiming: the thumb on the stick first
check("touching the stick (the gyro moves the cursor): paused", not pull(s, buttons=1 << BTN["RS_TOUCH"]))
s.run(50, ax=0, ay=0, az=G, buttons=0)
check("let go again (50 ms later): works", pull(s))
s = setup(0x80, act_mode=0)
check("activation 'always': paused all the time the gyro mouse is on", not pull(s))
s.cfg("gm_enabled", 0); s.run(50, ax=0, ay=0, az=G)
check("gyro mouse off: works", pull(s))
s = setup(0)
s.run(100, ax=0, ay=0, az=G, buttons=1 << BTN["RS_TOUCH"])
check("without the option: works while touching too", pull(s, buttons=1 << BTN["RS_TOUCH"]))
print("\n%d passed, %d failed" % (ok, fail))
