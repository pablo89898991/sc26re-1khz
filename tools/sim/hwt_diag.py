"""Diagnostic hardware cases: how many remapper passes the controller makes (mouse at the 127-per-report
limit). python3 hwt_diag.py -> web/hwdiag.json; hwtest.html?set=hwdiag"""
import json, os
import hwt
from simlib import *

def case(name, what, p, steps):
    c = dict(name=name, what=what, profile=bytes(p.b).hex(), script=hwt.script_bytes(steps).hex(),
             ms=sum(ms for ms, _ in steps), snap=True, tol=8, rel=0.06, slot=hwt.SLOT, extra={})
    c["sim"] = hwt.sim_run(c)["log"]
    return c

cs = []
p = Profile(); p.analog(0, MOUSE, speed=60000); p.analog(1, MOUSE, speed=60000)
cs.append(case("diag_pass_rate_2sticks", "both sticks as mouse: 120 counts/ms asked, at most 127 per pass", p,
               [(60, {}), (100, dict(lsx=32767, rsx=32767)), (300, dict(lsx=32767, rsx=32767)), (100, {})]))
p = Profile(); p.analog(0, MOUSE, speed=60000)
cs.append(case("diag_pass_rate_ls", "LS mouse far past 127 counts per pass: dx / 127 = passes",
               p, [(60, {}), (100, dict(lsx=32767)), (300, dict(lsx=32767)), (100, {})]))
p = Profile(); p.analog(0, MOUSE, speed=60000)
cs.append(case("diag_pass_rate_ls_touch", "the same with the right pad touched and moving (pad scans wake the loop)",
               p, [(60, {})] + [(10, dict(lsx=32767, buttons=1 << 21, rpx=-20000 + 2000 * i)) for i in range(20)] + [(100, {})]))
p = Profile(); p.analog(1, FLICK)
cs.append(case("diag_flick_180", "RS flick stick, 180 degrees: the turn's counts per step", p,
               [(60, {}), (20, dict(rsy=-32767)), (20, dict(rsy=-32767)), (20, dict(rsy=-32767)), (20, dict(rsy=-32767)), (200, dict(rsy=-32767)), (100, {})]))
p = Profile(); p.shift(0, BTN["A"]); p.shift(1, BTN["B"])
st = [(60, {})]
for i in range(30): st.append((7 + (i * 5) % 11, dict(A=1) if i % 2 == 0 else dict(B=1)))
st.append((50, {}))
c = case("diag_pass_gaps", "shift by A / B in steps of 7..17 ms: each change shows up in the first pass after the step edge", p, st)
c["snap"] = False; c["tol"] = 2
c["sim"] = hwt.sim_run(c)["log"]; cs.append(c)
p = Profile(); p.shift(5, None); p.b[328 + 20 * 5] = 31   # RPAD shifted by the LT soft pull
st = [(60, {})]
for i in range(30): st.append((9 + (i * 5) % 11, dict(tl=32767) if i % 2 == 0 else {}))
st.append((50, {}))
c = case("diag_soft_shift", "shift by the LT soft pull: how many passes after the pull it engages", p, st)
c["snap"] = False; c["tol"] = 2
c["sim"] = hwt.sim_run(c)["log"]; cs.append(c)
for c in cs: print(c["name"], [(e["t"], e["dx"]) for e in c["sim"]])
doc = dict(version=2, slot=hwt.SLOT, cfg=hwt.cfg_names(), cases=cs)
json.dump(doc, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../web/hwdiag.json"), "w"), separators=(",", ":"))
