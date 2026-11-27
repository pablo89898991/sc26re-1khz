"""Accelerometer tilt -> stick (v51.8): sc26re/acc/<profile>."""
import struct, math
from simlib import *

def acc_blob(stick=0, flags=0, sens=0, dz=0, btn=0, bsens=0, gsens=0, gest=None):
    g = bytearray(14)
    for k, acts in (gest or {}).items():
        for j, a in enumerate(acts): g[2 * k + j] = a
    return struct.pack("<BBHHBBHH", stick, flags, sens, dz, btn, 0, bsens, gsens) + bytes(g) + bytes(6)

def put_acc(s, slot, blob):
    for o in range(0, len(blob), 40):
        assert s.feature(0xEE, "sc26re/acc/%d/%x" % (slot, o), blob[o:o + 40])[0] == 0
    assert s.feature(0xEF, "sc26re/acc/%d" % slot)[0] == 0

def get_acc(s, slot):
    out = b""
    while len(out) < 32:
        d = s.read("sc26re/acc/%d/%x" % (slot, len(out)))
        assert d, len(out)
        out += d
    return out[:32]

G = 16384
def tilt(fwd=0.0, side=0.0):
    """gravity for a forward / sideways tilt (degrees) from flat (0, 0, G)"""
    f, sd = math.radians(fwd), math.radians(side)
    x, y, z = -math.sin(sd), -math.sin(f) * math.cos(sd), math.cos(f) * math.cos(sd)
    return dict(ax=int(round(x * G)), ay=int(round(y * G)), az=int(round(z * G)))

ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)

def near(v, want, tol=400): return abs(v - want) <= tol

def fresh(blob, prof=None):
    s = Sim(); p = prof or Profile(); s.use(p); put_acc(s, 0, blob); s.run(3, **tilt()); return s

# storage
s = Sim(); s.use(Profile())
b = acc_blob(1, 0x85, 150, 1200)
put_acc(s, 0, b)
check("read back", get_acc(s, 0) == b)
check("other profile untouched", get_acc(s, 1) == bytes(32))
s.reboot(); check("survives a reboot", get_acc(s, 0) == b)
check("no global slot", s.read("sc26re/acc/g/0") is None)

# tilt -> left stick, 100 % = full at 45 degrees
s = fresh(acc_blob(1))
s.run(5, **tilt(fwd=22.5)); check("forward 22.5 deg = half up", near(s.out["lsy"], 16384) and near(s.out["lsx"], 0), s.out)
s.run(5, **tilt(side=45)); check("sideways 45 deg = full right", near(s.out["lsx"], 32767) and near(s.out["lsy"], 0), s.out)
s.run(5, **tilt(side=-60)); check("past full: clamps", s.out["lsx"] == -32767, s.out["lsx"])
s.run(5, **tilt(fwd=-11.25)); check("back 11.25 deg = quarter down", near(s.out["lsy"], -8192), s.out["lsy"])
s.run(5, **tilt()); check("level = 0", abs(s.out["lsx"]) < 50 and abs(s.out["lsy"]) < 50, s.out)
check("right stick untouched", s.out["rsx"] == 0 and s.out["rsy"] == 0)
# sensitivity
s = fresh(acc_blob(1, 0, 200)); s.run(5, **tilt(fwd=22.5))
check("200 %: full at 22.5 deg", near(s.out["lsy"], 32767, 100), s.out["lsy"])
s = fresh(acc_blob(1, 0, 50)); s.run(5, **tilt(fwd=45))
check("50 %: half at 45 deg", near(s.out["lsy"], 16384), s.out["lsy"])
# dead zone 20 %
s = fresh(acc_blob(1, 0, 100, 2000))
s.run(5, **tilt(fwd=8)); check("dead zone: small tilt = 0", s.out["lsy"] == 0, s.out["lsy"])
s.run(5, **tilt(fwd=27)); check("dead zone: 60 % tilt = 50 %", near(s.out["lsy"], 16384), s.out["lsy"])
# invert / swap / right stick
s = fresh(acc_blob(2, 0x01 | 0x02)); s.run(5, **tilt(fwd=22.5, side=22.5))
check("right stick, both inverted", near(s.out["rsx"], -16384, 1400) and near(s.out["rsy"], -16384, 1400) and s.out["lsx"] == 0, s.out)
s = fresh(acc_blob(1, 0x04)); s.run(5, **tilt(fwd=22.5))
check("swap: forward on X", near(s.out["lsx"], 16384) and near(s.out["lsy"], 0), s.out)
# added to the physical stick
s = fresh(acc_blob(1)); s.run(5, lsx=10000, **tilt(side=22.5))
check("added to the stick", near(s.out["lsx"], 26384), s.out["lsx"])
s.run(5, lsx=0)
# neutral = how it is held when the profile becomes active
s = Sim(); s.use(Profile()); put_acc(s, 0, acc_blob(1)); s.cfg("remap_on", 0); s.run(5, **tilt(fwd=30))
s.cfg("remap_on", 1); s.run(5, **tilt(fwd=30))
check("neutral where held at activation", abs(s.out["lsy"]) < 50, s.out["lsy"])
s.run(5, **tilt(fwd=7.5)); check("level again = back", near(s.out["lsy"], -16384, 700), s.out["lsy"])
# IMU kept on only while needed
s = fresh(acc_blob(1)); check("IMU kept on with tilt -> stick", s.lib.sim_imu_forced() & 2)
s.cfg("remap_on", 0); s.run(3); check("remapper off: IMU let go", not (s.lib.sim_imu_forced() & 2))
s.cfg("remap_on", 1); s.run(3); check("remapper on: IMU on again", s.lib.sim_imu_forced() & 2)
put_acc(s, 0, acc_blob(0)); s.run(3); check("tilt off: IMU let go", not (s.lib.sim_imu_forced() & 2))
s = Sim(); s.use(Profile()); s.run(3); check("profile without it: IMU not forced", not (s.lib.sim_imu_forced() & 2))
# gyro mouse on: paused with the flag, working without it (accel hidden from the pad report)
s = fresh(acc_blob(1, 0x80)); s.lib.sim_gm_set(1); s.run(5, **tilt(side=22.5))
check("paused while the gyro is on", s.out["lsx"] == 0, s.out["lsx"])
s.lib.sim_gm_set(0); s.run(5, **tilt(side=22.5)); check("gyro off: works again", near(s.out["lsx"], 16384), s.out["lsx"])
s = fresh(acc_blob(1, 0)); s.lib.sim_gm_set(1); s.run(5, **tilt(side=22.5))
check("without the flag: works while the gyro is on", near(s.out["lsx"], 16384), s.out["lsx"])
# another profile without it
s = fresh(acc_blob(1)); s.upload(1, Profile()); s.cfg("remap_profile", 1); s.run(5, **tilt(side=22.5))
check("other profile: nothing", s.out["lsx"] == 0, s.out["lsx"])

# ---- tilt -> buttons (the 4 tilt sources), 100 % = 20 degrees
def bprof():
    p = Profile()
    for s_, k in (("GYRO_UP", "w"), ("GYRO_DOWN", "s"), ("GYRO_LEFT", "a"), ("GYRO_RIGHT", "d")): p.bind(s_, key(k))
    return p
class KB:
    """keys held at the PC, from the keyboard reports"""
    def __init__(self, s): self.s, self.now = s, ()
    def run(self, ms, **kw):
        k = self.s.run(ms, **kw).kbd()
        if k: self.now = k[-1]
        return self
    def down(self, c): return key(c) in self.now
s = fresh(acc_blob(btn=1), bprof()); kb = KB(s)
e = kb.run(20, **tilt(fwd=25)); check("forward 25 deg: Tilt forward pressed", e.down("w"), e)
e = kb.run(20, **tilt(fwd=18.5)); check("hysteresis: 18.5 deg still held", e.down("w"), e)
e = kb.run(20, **tilt(fwd=16)); check("back under 17 deg: released", not e.down("w"), e)
e = kb.run(20, **tilt(side=-25)); check("left 25 deg: Tilt left", e.down("a"), e)
kb.run(20, **tilt())
e = kb.run(20, **tilt(fwd=25, side=30)); check("4-way: only the bigger tilt (right)", e.down("d") and not e.down("w"), e)
kb.run(20, **tilt())
s = fresh(acc_blob(btn=2), bprof()); kb = KB(s)
e = kb.run(20, **tilt(fwd=25, side=30)); check("8-way: both", e.down("d") and e.down("w"), e)
s = fresh(acc_blob(btn=1, bsens=200), bprof()); kb = KB(s)
e = kb.run(20, **tilt(fwd=12)); check("200 %: 12 deg is enough", e.down("w"), e)
s = fresh(acc_blob(btn=1, bsens=50), bprof()); kb = KB(s)
e = kb.run(20, **tilt(fwd=30)); check("50 %: 30 deg not enough", not e.down("w"), e)
e = kb.run(20, **tilt(fwd=45)); check("50 %: 45 deg is", e.down("w"), e)
s = fresh(acc_blob(btn=1, flags=0x80), bprof()); kb = KB(s); s.lib.sim_gm_set(1)
e = kb.run(20, **tilt(fwd=30)); check("buttons paused while the gyro is on", not e.down("w"), e)
s.lib.sim_gm_set(0); e = kb.run(20, **tilt(fwd=30)); check("gyro off: pressed", e.down("w"), e)
s = fresh(acc_blob(btn=1), bprof()); check("IMU kept on for tilt buttons", s.lib.sim_imu_forced() & 2)
s = fresh(acc_blob(stick=1, btn=1), bprof()); kb = KB(s); e = kb.run(20, **tilt(fwd=25))
check("stick and buttons together", e.down("w") and near(s.out["lsy"], int(32767 * 25 / 45)), (s.out["lsy"],))

# ---- gestures: jolts along the controller's axes (x right, y forward, z up), 100 % = 1.5 g
# (scale-free: the firmware measures in units of its gravity average; 1 g = 8192 here so 3 g fits the report)
G = 8192
FWD, BACK, LEFT, RIGHT, UP, DOWN, SHAKE = range(7)
GK = {FWD: "w", BACK: "s", LEFT: "a", RIGHT: "d", UP: "e", DOWN: "q", SHAKE: "x"}
def gblob(**kw): return acc_blob(gest={k: (key(v),) for k, v in GK.items()}, **kw)
def gfresh(blob):
    s = Sim(); s.use(Profile()); put_acc(s, 0, blob); s.run(600, ax=0, ay=0, az=G); return s
def jab(s, axis, g, ms=30):
    """a jolt of g (gravity units) along axis, then the stop: the same the other way"""
    base = dict(ax=0, ay=0, az=G); a = dict(base); a[axis] += int(g * G)
    b = dict(base); b[axis] -= int(g * G)
    k = s.run(ms, **a).kbd() + s.run(ms, **b).kbd() + s.run(500, **base).kbd()
    return k
def taps(k):
    out, prev = [], ()
    for st in k:
        for c in st:
            if c not in prev: out.append(c)
        prev = st
    return out
inv = {key(v): v for v in GK.values()}
for axis, g, want, name in (("ay", 2.0, "w", "forward"), ("ay", -2.0, "s", "back"), ("ax", 2.0, "d", "right"),
                            ("ax", -2.0, "a", "left"), ("az", 2.0, "e", "up"), ("az", -2.0, "q", "down")):
    s = gfresh(gblob()); t = [inv.get(c, c) for c in taps(jab(s, axis, g))]
    check("gesture %s: only %s, once (the stop is not a gesture)" % (name, want), t == [want], t)
s = gfresh(gblob()); t = taps(jab(s, "ay", 1.2)); check("1.2 g at 100 %: nothing", t == [], t)
s = gfresh(gblob(gsens=200)); t = [inv.get(c, c) for c in taps(jab(s, "ay", 1.0))]; check("200 %: 1 g fires", t == ["w"], t)
s = gfresh(gblob(gsens=50)); t = taps(jab(s, "ay", 2.0)); check("50 %: 2 g is not enough", t == [], t)
# the tap is short
s = gfresh(gblob()); k = jab(s, "ay", 2.0)
s2 = gfresh(gblob()); held = 0
for ms, kw in ((30, dict(ax=0, ay=int(2 * G), az=G)), (30, dict(ax=0, ay=-int(2 * G), az=G))) + tuple((1, dict(ax=0, ay=0, az=G)) for _ in range(300)):
    s2.run(ms, **kw); kb = s2.run(0).kbd() if False else None
check("tap: pressed then let go", taps(k) == [key("w")] and (not k or key("w") not in k[-1]), k[-3:])
# shake: back and forth
s = gfresh(gblob()); ev = []
for i in range(8):
    a = dict(ax=int((2 if i % 2 == 0 else -2) * G), ay=0, az=G)
    ev += s.run(60, **a).kbd()
ev += s.run(500, ax=0, ay=0, az=G).kbd()
t = [inv.get(c, c) for c in taps(ev)]
check("shake: fires once, and at most the first jolt's direction", t.count("x") == 1 and len([x for x in t if x != "x"]) <= 1, t)
# a long shake is still one shake; a second shake after a pause fires again
s = gfresh(gblob()); ev = []
for i in range(16): ev += s.run(60, ax=int((2 if i % 2 == 0 else -2) * G), ay=0, az=G).kbd()
ev += s.run(1000, ax=0, ay=0, az=G).kbd()
for i in range(8): ev += s.run(60, ax=int((2 if i % 2 == 0 else -2) * G), ay=0, az=G).kbd()
ev += s.run(500, ax=0, ay=0, az=G).kbd()
t = [inv.get(c, c) for c in taps(ev)]
check("long shake = 1, then a pause and another shake = 2, no directions in between", t == ["d", "x", "d", "x"], t)
s = gfresh(acc_blob(gest={RIGHT: (key("d"),)})); ev = []
for i in range(8):
    ev += s.run(60, ax=int((2 if i % 2 == 0 else -2) * G), ay=0, az=G).kbd()
t = [inv.get(c, c) for c in taps(ev)]; check("shaking with only Right bound: Right once", t == ["d"], t)
# two jabs (left, then right 0.8 s later): each with its stop is 4 jolts, but not one movement: no shake
s = gfresh(gblob()); ev = []
for ax_, g_ in (("ax", -2.0), ("ax", 2.0)):
    # a hand jab: the push, the stop, and a small rebound past the threshold
    base = dict(ax=0, ay=0, az=G); a = dict(base); a[ax_] += int(g_ * G); b = dict(base); b[ax_] -= int(g_ * G)
    r_ = dict(base); r_[ax_] += int(g_ * 0.85 * G)
    ev += s.run(40, **a).kbd() + s.run(40, **b).kbd() + s.run(40, **r_).kbd() + s.run(680, **base).kbd()
t = [inv.get(c, c) for c in taps(ev)]; check("jab left, jab right 0.8 s later: left, right, no shake", t == ["a", "d"], t)
# a slower shake (about 3 per second, 150 ms a side) is still a shake
s = gfresh(gblob()); ev = []
for i in range(8): ev += s.run(150, ax=int((2 if i % 2 == 0 else -2) * G), ay=0, az=G).kbd()
ev += s.run(500, ax=0, ay=0, az=G).kbd()
t = [inv.get(c, c) for c in taps(ev)]; check("slower shake (150 ms a side): a shake", t.count("x") == 1, t)

# recorded on the controller (v51.8 test 11): a hand shake tilting left and right is slow, jolts just
# over the threshold 0.5..1 s apart: a shake. Tilt jolts last ~60 ms and have no quick stop
s = gfresh(gblob()); ev = []
for i, gap in enumerate((513, 969, 682, 578, 0)):
    ev += s.run(60, ax=int((1.6 if i % 2 else -1.6) * G), ay=0, az=G).kbd()
    if gap: ev += s.run(gap - 60, ax=0, ay=0, az=G).kbd()
ev += s.run(1200, ax=0, ay=0, az=G).kbd()
t = [inv.get(c, c) for c in taps(ev)]; check("slow shake (0.5..1 s apart, as recorded): a shake", t.count("x") == 1, t)
# two slow single jolts and a pause: nothing more than their directions
s = gfresh(gblob()); ev = []
for i in range(3):
    ev += s.run(60, ax=int((1.6 if i % 2 else -1.6) * G), ay=0, az=G).kbd() + s.run(600, ax=0, ay=0, az=G).kbd()
ev += s.run(1500, ax=0, ay=0, az=G).kbd()
t = [inv.get(c, c) for c in taps(ev)]; check("three slow jolts: no shake", "x" not in t, t)
# two actions on one gesture
s = gfresh(acc_blob(gest={FWD: (0xE1, key("w"))})); k = jab(s, "ay", 2.0)
check("two actions: shift + w together", any(0xE1 in st and key("w") in st for st in k), k)
# paused while the gyro is on
s = gfresh(gblob(flags=0x80)); s.lib.sim_gm_set(1); t = taps(jab(s, "ay", 2.0)); check("paused while the gyro is on", t == [], t)
s.lib.sim_gm_set(0); s.run(600, ax=0, ay=0, az=G); t = taps(jab(s, "ay", 2.0)); check("gyro off: works again", t == [key("w")], t)
s = gfresh(gblob()); check("IMU kept on for gestures", s.lib.sim_imu_forced() & 2)
s = gfresh(acc_blob()); check("nothing bound: IMU not forced", not (s.lib.sim_imu_forced() & 2))
# an IMU restart (gyro range change) makes the sample sizes jump: no gestures while it settles
s = gfresh(gblob()); s.lib.sim_imu_settling(1); t = taps(jab(s, "ay", 2.0)); check("IMU settling: a jolt is ignored", t == [], t)
s.run(50, ax=0, ay=int(1.5 * G), az=G); s.lib.sim_imu_settling(0); s.run(400, ax=0, ay=0, az=G)
t = taps(jab(s, "ay", 2.0)); check("after settling: works, nothing left over", t == [key("w")], t)
# a slow tilt is not a gesture
s = gfresh(gblob()); ev = []
for d in range(0, 61, 2): ev += s.run(30, **tilt(fwd=d)).kbd()
check("slow tilt: no gesture", taps(ev) == [], taps(ev))

# while the IMU settles (range change: the driver passes other ranges), tilt keeps what it had just
# before: measured on the controller, no sample, half and then 2.8 times gravity, later near zero
s = fresh(acc_blob(1, btn=1)); s.run(5, **tilt(fwd=22.5)); before = (s.out["lsx"], s.out["lsy"], s.out["buttons"])
s.lib.sim_imu_settling(1); seen = set()
for smp in (dict(ax=0, ay=0, az=0), dict(ax=0, ay=-3135, az=7568), dict(ax=-300, ay=-32767, az=32767), dict(ax=0, ay=-50, az=600)):
    s.run(20, **smp); seen.add((s.out["lsx"], s.out["lsy"], s.out["buttons"]))
check("settling: tilt stick and tilt presses held", seen == {before}, (before, seen))
s.lib.sim_imu_settling(0); s.run(5, **tilt(side=45))
check("after settling: tilt follows again", near(s.out["lsx"], 32767) and near(s.out["lsy"], 0), s.out)
s = fresh(acc_blob(1)); s.lib.sim_imu_settling(1); s.run(20, ax=0, ay=-32767, az=32767)
check("settling from the start: nothing", s.out["lsx"] == 0 and s.out["lsy"] == 0, s.out)
s.lib.sim_imu_settling(0)

print("\n%d passed, %d failed" % (ok, fail))
raise SystemExit(1 if fail else 0)
