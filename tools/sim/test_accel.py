"""Accelerometer tilt -> stick (v51.8): sc26re/acc/<profile>."""
import struct, math
from simlib import *

def acc_blob(stick=0, flags=0, sens=0, dz=0):
    return struct.pack("<BBHH", stick, flags, sens, dz) + bytes(26)

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

print("\n%d passed, %d failed" % (ok, fail))
raise SystemExit(1 if fail else 0)
