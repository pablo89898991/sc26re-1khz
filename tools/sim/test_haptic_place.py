"""v51.4: the remapper's binding haptics on trigger sources follow trig_haptic (0 grips, 1 trackpads, 2 both)."""
from simlib import *
ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)
H = lambda e: [tuple(int(v) for v in x.split()[2:]) for x in e.haptics()]
for place, want in ((0, 8), (1, 2)):           # right side: pad 0x02, grip 0x08
    p = Profile(); p.bind("RT_CLICK", key("a"), haptic=2); p.bind("A", key("b"), haptic=2)
    s = Sim(); s.use(p); s.cfg("trig_haptic", place); s.run(5)
    h = H(s.run(50, RT_CLICK=1)); s.run(50, RT_CLICK=0)
    a = H(s.run(50, A=1))
    check("full pull haptic, trig_haptic=%d" % place, h and all(x[1] == want for x in h), h)
    check("a face button stays on the trackpads (trig_haptic=%d)" % place, a and all(x[1] in (1, 2, 3) for x in a), a)
# both: the grip and (80 ms later in the firmware) the trackpad, same level
p = Profile(); p.bind("RT_CLICK", key("a"), haptic=2)
s = Sim(); s.use(p); s.cfg("trig_haptic", 2); s.run(5)
h = H(s.run(50, RT_CLICK=1))
check("both: grip and trackpad, same level", sorted(h) == [(2, 2), (2, 8)], h)
print(); print("%d passed, %d failed" % (ok, fail))
