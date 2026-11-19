"""Four different profiles for the 3D overlay test, uploaded to the virtual controller
(sim_server.py on 8765) the way web/remap.html saves them. Prints what each one does."""
import json, struct, sys, urllib.request
from simlib import *

SRV = "http://127.0.0.1:%s" % (sys.argv[1] if len(sys.argv) > 1 else "8765")


def feature(op, path, data=b""):
    p = path.encode()
    req = bytearray(64)
    req[0], req[1] = op, len(p) + 1 + len(data)
    req[2:2 + len(p)] = p
    req[3 + len(p):3 + len(p) + len(data)] = bytes(data)
    r = urllib.request.urlopen(urllib.request.Request(SRV + "/feature", data=req.hex().encode(), method="POST"))
    return json.loads(r.read())["r"]


def upload(slot, prof):
    for o in range(0, len(prof.b), 40):
        assert feature(0xEE, "sc26re/rmp/%d/%x" % (slot, o), prof.b[o:o + 40]) == 0, o
    assert feature(0xEF, "sc26re/rmp/%d" % slot) == 0


# 1: keyboard and mouse, unbound inputs hidden from Steam
p1 = Profile("keys"); p1.flags(PF_BLOCK)
p1.bind("A", key("space")); p1.bind("X", PAD + BTN["X"]); p1.bind("LT_CLICK", MOUSE_L)
p1.analog(1, MOUSE, speed=100)                        # right stick moves the mouse
p1.analog(2, MOUSE, speed=100)                        # left trackpad too
# 2: Xbox pad buttons (no Xbox pad at the PC: they reach Steam), A and B swapped
p2 = Profile("xbox")
p2.bind("A", XBOX + BTN["B"]); p2.bind("B", XBOX + BTN["A"]); p2.bind("Y", XBOX + BTN["Y"], mode=M_TURBO)
p2.analog(0, XSTICK)
# 3: stick and pad modes, gyro tilt as buttons
p3 = Profile("modes"); p3.flags(PF_GYRO); p3.gyx(mode=1, tilt_dz=15)
p3.analog(0, DPAD4); p3.bind("LS_UP", key("w")); p3.bind("LS_DOWN", key("s"))
p3.analog(2, RADIAL); p3.analog(3, FLICK)
p3.bind("GYRO_UP", key("1")); p3.bind("B", MACRO + 0); p3.macro(0, [(key("a"), 3), (key("b"), 3)])
# 4: a mode shift on L4, trigger modes, its own gyro range, the gyro mouse on (Steam gets no motion then)
p4 = Profile("shift"); p4.flags(PF_GYRO | PF_GYRO_ON); struct.pack_into("<h", p4.b, 124, 500)
p4.shift(0, BTN["L4"], mode=MOUSE, speed=80)
p4.bind("A", key("e"), a1=key("f")); p4.bind("RB", key("r"), mode=M_HOLD)
p4.trig_mode(0, 1); p4.trig_soft(1, 40)

for i, p in enumerate((p1, p2, p3, p4)):
    upload(i, p)
assert feature(0xEE, "sc26re/remap_on", struct.pack("<h", 0)) == 0 and feature(0xEF, "sc26re/remap_on") == 0
print("uploaded keys, xbox, modes, shift")
