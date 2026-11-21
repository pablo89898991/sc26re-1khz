# Outer ring "ring key first" (REMAP_AF_RING_FIRST, sticks in 4 / 8-way): when the ring comes on
# while a direction is held, the direction lets go for one pass so the ring key (Shift) is down first.
import sys
from simlib import Sim, Profile

ok = bad = 0
def check(name, cond, info=''):
    global ok, bad
    if cond: ok += 1
    else: bad += 1; print('FAIL', name, info)

def run(first, steps, invert=False):
    s = Sim(); p = Profile('r')
    p.analog(0, 2, flags=(8 if first else 0) | (2 if invert else 0), dz_in=800, ring=8600)
    p.bind('LS_UP', 0x1A); p.bind('LS_RIGHT', 0x07); p.bind(60, 0xE1)
    s.use(p, 0); s.cfg('remap_on', 1); s.run(20)
    seq = []
    for x, y in steps: seq += s.run(1, lsx=x, lsy=y).kbd()
    return seq

ramp = [(0, 3000), (0, 15000), (0, 25000), (0, 32767), (0, 32767)]
check('off: W then W + Shift', run(False, ramp) == [(26,), (26, 225)], run(False, ramp))
check('on: W, Shift alone, then W + Shift', run(True, ramp) == [(26,), (225,), (26, 225)], run(True, ramp))
jump = [(0, 0), (0, 32767), (0, 32767)]
check('on, straight to the edge: Shift first', run(True, jump) == [(225,), (26, 225)], run(True, jump))
turn = ramp + [(23170, 23170), (32767, 0), (32767, 0)]
seq = run(True, turn)
check('on, turning along the ring: no extra release', seq[-2:] == [(7, 26, 225), (7, 225)], seq)
inv = run(True, ramp, invert=True)
check('inner ring (invert): left alone', (225,) not in inv, inv)
print(f'{ok} passed, {bad} failed')
sys.exit(1 if bad else 0)
