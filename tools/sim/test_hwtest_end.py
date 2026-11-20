"""After a hardware test run the remapper lets go of what the test profile held (v51 test 66):
toggled keys, mouse buttons and the rest of a mouse move never reach the PC afterwards."""
import struct
import hwt
from simlib import *
ok = fail = 0
def check(n, c, i=""):
    global ok, fail
    if c: ok += 1; print("PASS", n)
    else: fail += 1; print("FAIL", n, i)

p = Profile()
p.bind("A", key("h"), mode=M_TOGGLE)
p.bind("B", (KEY_LALT if "KEY_LALT" in globals() else key("j")), mode=M_TOGGLE)
p.bind("X", MOUSE_L, mode=M_TOGGLE)
s = Sim(); s.use(p, 3); s.lib.sim_xbox(0); s.run(50)
steps = [(30, {}), (40, dict(A=1, B=1, X=1)), (40, {})]
sb = hwt.script_bytes(steps)
for o in range(0, len(sb), 40): s.feature(0xEE, "sc26re/hwt", b"\x01" + struct.pack("<H", o) + sb[o:o + 40])
s.feature(0xEE, "sc26re/hwt", bytes([2, len(sb) // 42, 0]))
s.run(60)
h = s.host(); check("during the run the toggles are on (the simulator's PC sees them)", any(h["kbd"][2:]) and h["mb"] & 1, h)
s.run(100)
h = s.host(); check("after the run: no key held", not any(h["kbd"]), h["kbd"].hex())
check("after the run: no mouse button held", not h["mb"], h)
s.run(50, A=1); s.run(50, A=0)
check("the remapper still works afterwards (A toggles h on)", key("h") in s.host()["kbd"][2:], s.host()["kbd"].hex())
print("\n%d passed, %d failed" % (ok, fail))
