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
for c in cs: print(c["name"], [(e["t"], e["dx"]) for e in c["sim"]])
doc = dict(version=2, slot=hwt.SLOT, cfg=hwt.cfg_names(), cases=cs)
json.dump(doc, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../web/hwdiag.json"), "w"), separators=(",", ":"))
