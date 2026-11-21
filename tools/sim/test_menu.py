# sc26re/osd: the on-screen menu state the overlay page draws (v51 test 84)
from simlib import Sim, Profile, RADIAL, GRID, BTN

ok = bad = 0
def check(name, cond, info=''):
    global ok, bad
    if cond: ok += 1
    else: bad += 1; print('FAIL', name, info)

def menu(s):
    b = s.read("sc26re/osd"); assert b and len(b) == 52, b
    ms = []
    for m in range(b[3]):
        o = b[4 + 24 * m: 28 + 24 * m]
        ms.append(dict(inp=o[0] & 15, kind=o[0] >> 4, n=o[1], sel=(o[2] ^ 0x80) - 0x80, src=list(o[4:4 + o[1]]), act=list(o[14:14 + o[1]])))
    return b[0], (b[1] ^ 0x80) - 0x80, b[2], ms

def fresh(p, slot=0):
    s = Sim(); s.use(p, slot); s.cfg('remap_profile', slot); s.cfg('remap_on', 1); s.run(20); return s

# right pad radial, 4 slices: up, right, down, left = sources 42, 45, 43, 44
p = Profile('m'); p.analog(3, RADIAL, speed=4, dz_in=2500)
p.bind('RP_UP', 0x1E); p.bind('RP_RIGHT', 0x1F); p.bind('RP_DOWN', 0x20); p.bind('RP_LEFT', 0x21)
s = fresh(p)
prof, lay, sh, ms = menu(s)
check('nothing open', ms == [] and prof == 0 and lay == -1, (prof, lay, ms))
s.run(20, RP_TOUCH=1, rpx=28000, rpy=0)
prof, lay, sh, ms = menu(s)
check('radial open on touch', len(ms) == 1 and ms[0]['inp'] == 1 and ms[0]['kind'] == 1 and ms[0]['n'] == 4, ms)
check('right slice chosen', ms and ms[0]['sel'] == 1, ms)
check('slice sources (radial order)', ms and ms[0]['src'] == [42, 51, 45, 53], ms)
s.run(20, RP_TOUCH=1, rpx=0, rpy=0)
check('centre: open, nothing chosen', menu(s)[3][0]['sel'] == -1, menu(s))
s.run(20, RP_TOUCH=0)
check('closed on lift', menu(s)[3] == [], menu(s))

# left stick radial, 4 slices: up, right, down, left
p = Profile('s'); p.analog(0, RADIAL, dz_in=3000); p.anx(0, slices=4)
p.bind('LS_UP', 0x1E); p.bind('LS_RIGHT', 0x1F); p.bind('LS_DOWN', 0x20); p.bind('LS_LEFT', 0x21)
s = fresh(p)
s.run(30, lsx=0, lsy=-30000)
ms = menu(s)[3]
check('stick radial: 4 slices, down chosen', ms and ms[0]['inp'] == 2 and ms[0]['sel'] == 2, ms)
check('stick 4 slices: up, right, down, left sources + actions', ms and ms[0]['src'] == [30, 33, 31, 32] and ms[0]['act'] == [0x1E, 0x1F, 0x20, 0x21], ms)
s.run(30, lsx=0, lsy=0)
check('stick back: closed', menu(s)[3] == [], menu(s))

# left pad grid
p = Profile('g'); p.analog(2, GRID); p.bind('LP_CENTER', 0x06); p.bind('LP_UP', 0x1A)
s = fresh(p)
s.run(20, LP_TOUCH=1, lpx=0, lpy=28000)
ms = menu(s)[3]
check('grid: top middle', ms and ms[0]['kind'] == 2 and ms[0]['n'] == 9 and ms[0]['sel'] == 1, ms)
check('grid: centre and up actions', ms and ms[0]['act'][4] == 0x06 and ms[0]['act'][1] == 0x1A and ms[0]['src'][4] == 64, ms)

# profile shown
s = fresh(Profile('x'), 2)
check('profile 3 shown', menu(s)[0] == 2, menu(s))

print('menu: %d ok, %d failed' % (ok, bad))
raise SystemExit(1 if bad else 0)
