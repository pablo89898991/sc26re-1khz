"""Real Steam Input layouts (community .vdf files) imported with web/vdf.js and played on the
real remap.c over the three links: USB cable, Puck (2.4 GHz) and Bluetooth.
The files: tools/sim/vdf_samples/ (or VDF_DIR); a missing file skips its tests."""
import os, sys, traceback, zlib
SEED = int(os.environ.get("SEED", "0"))
from simlib import *
from test_vdf import vdf, prof_of
from test_remap import touch, lift

DIR = os.environ.get("VDF_DIR", "/mnt/user-data/uploads")
FILES = dict(stalker="5d181df4-3728087656_controller_config.vdf", puzzle="f802a6dc-3724992272_controller_config.vdf",
             deck="592293e0-3759492888_controller_config.vdf", deadlock="0289a2b4-3737914488_controller_config.vdf")
LINKS = [(0, "USB"), (1, "Puck"), (2, "BT")]
TESTS = []
def test(f): TESTS.append(f); return f

_cache = {}
def load(name, **opts):
    k = (name, tuple(sorted(opts.items())))
    if k not in _cache:
        path = os.path.join(DIR, FILES[name])
        if not os.path.exists(path): _cache[k] = None
        else: _cache[k] = vdf(open(path, encoding="utf-8").read(), opts)
    return _cache[k]

def sim(r, link, slot=0, xbox=1, loss=0, ci=15):
    s = Sim(); s.transport(link, one=1 if link == 1 else 0, loss=loss, ci=ci); s.lib.sim_xbox(xbox)   # Puck: one queued report per payload, as transport_esb.c
    for k, x in enumerate(r["profiles"]):
        if x: s.upload(k, prof_of(x))
    s.cfg("remap_profile", slot); s.cfg("remap_on", 1); s.run(30)
    return s

def keys(s): return set(s.host()["kbd"][2:]) - {0}
def mb(s): return s.host()["mb"]

def pad_btn(s, bit):
    """the gamepad button: the Xbox pad over USB (pad_out on), else the Steam pad"""
    return bool(s.lib.sim_xbox_buttons() & (1 << bit)) if s.lib.sim_xbox_live() else bool(s.out["buttons"] & (1 << bit))

def pad_trig(s, t):
    return s.lib.sim_xbox_trigger(t) * 128 if s.lib.sim_xbox_live() else s.out["tr" if t else "tl"]

def pad_stick(s, i):
    if s.lib.sim_xbox_live(): return s.lib.sim_xbox_stick(i, 0), s.lib.sim_xbox_stick(i, 1)
    return (s.out["rsx"], s.out["rsy"]) if i else (s.out["lsx"], s.out["lsy"])

def each_link(f):
    def run():
        for link, nm in LINKS:
            for xbox in ((1, 0) if link == 0 else (1,)):
                try: f(link, xbox)
                except AssertionError as e: raise AssertionError(f"[{nm}{'' if xbox else ', Steam pad only'}] {e}") from None
    run.__name__ = f.__name__
    return run

def skip(name):
    if load(name) is None: print(f"     ({FILES[name]} not found: skipped)"); return True

# ---------------- STALKER 2: gamepad + keys + mouse + touch menu + gyro ----------------
@test
@each_link
def stalker_buttons(link, xbox):
    if skip("stalker"): return
    s = sim(load("stalker"), link, xbox=xbox)
    for btn, bit in (("A", 0), ("B", 1), ("X", 2), ("Y", 3), ("LB", 19), ("RB", 9), ("MENU", 6), ("VIEW", 14), ("L3", 15), ("R3", 5)):
        s.run(30, **{btn: 1}); assert pad_btn(s, bit), (btn, hex(s.out["buttons"]), hex(s.lib.sim_xbox_buttons()))
        s.run(30, **{btn: 0}); assert not pad_btn(s, bit), ("stuck", btn)
    s.run(30, R5=1); assert pad_btn(s, 1), "R5 = B (crouch)"; s.run(30, R5=0)
    s.run(40, L4=1); assert key("z") in keys(s), keys(s); s.run(40, L4=0); s.run(60); assert key("z") not in keys(s)
    s.run(40, R4=1); assert key("c") in keys(s); s.run(40, R4=0); s.run(60)
    s.run(40, L5=1); assert mb(s) & 2, mb(s); s.run(40, L5=0); s.run(60); assert not mb(s) & 2

@test
@each_link
def stalker_sticks_and_triggers(link, xbox):
    if skip("stalker"): return
    s = sim(load("stalker"), link, xbox=xbox)
    s.run(40, lsx=32767); x, y = pad_stick(s, 0); assert x > 30000 and abs(y) < 500, (x, y)
    s.run(40, lsx=1000); x, y = pad_stick(s, 0); assert x == 0, ("dead zone 1600", x)
    s.run(40, lsx=0, rsy=-32767); x, y = pad_stick(s, 1); assert y < -30000, (x, y)
    s.run(40, rsy=0)
    s.run(40, tl=16000); v = pad_trig(s, 0); assert 12000 < v < 20000, ("analog LT", v)
    s.run(40, tl=32767, LT_CLICK=1); assert pad_trig(s, 0) > 32000 and pad_btn(s, 27)
    s.run(40, tl=0, LT_CLICK=0); assert pad_trig(s, 0) == 0 and not pad_btn(s, 27)
    s.run(40, tr=32767, RT_CLICK=1); assert pad_trig(s, 1) > 32000; s.run(40, tr=0, RT_CLICK=0)

@test
@each_link
def stalker_trackpads(link, xbox):
    if skip("stalker"): return
    s = sim(load("stalker"), link, xbox=xbox)
    # right pad: mouse, soft click = left mouse button
    h0 = s.host()["dx"]; touch(s, 1, 0, 0, 20)
    for k in range(25): touch(s, 1, k * 500, 0, 4)
    lift(s, 1); s.run(100); assert s.host()["dx"] - h0 > 20, s.host()["dx"] - h0
    # left pad: touch menu (radial), slice 0 = F5 (save), fires when the finger lifts
    touch(s, 0, 0, 28000, 60); seen = set()
    for _ in range(10): lift(s, 0, 3); seen |= keys(s)
    s.run(80)
    assert 0x3E in seen, seen
    assert not keys(s), keys(s)

@test
def stalker_gyro_settings():
    r = load("stalker")
    if r is None: return
    p = prof_of(r["profiles"][0])
    assert p.b[17] & 6 == 6 and struct.unpack_from("<h", p.b, 44)[0] == 1, "gyro to joystick, own settings"

# ---------------- Puzzle game (Steam Input API): mouse buttons, keys, mouse region ----------------
@test
@each_link
def puzzle_button_set(link, xbox):
    if skip("puzzle"): return
    r = load("puzzle")
    s = sim(r, link, slot=1, xbox=xbox)
    s.run(40, A=1); assert mb(s) & 1, mb(s); s.run(40, A=0); s.run(60); assert not mb(s) & 1
    s.run(40, B=1); assert mb(s) & 2; s.run(40, B=0); s.run(60)
    s.run(40, Y=1); assert mb(s) & 4, "middle button"; s.run(40, Y=0); s.run(60)
    s.run(40, LB=1); assert key("c") in keys(s); s.run(40, LB=0); s.run(60); assert not keys(s)

@test
@each_link
def puzzle_mouse_region(link, xbox):
    if skip("puzzle"): return
    s = sim(load("puzzle"), link, slot=1, xbox=xbox)
    s.log.clear(); h0 = s.host()["dx"]
    for k in range(30): touch(s, 0, -20000 + k * 1000, 5000, 5)
    lift(s, 0); s.run(100)
    absr = [e for e in s.log if e.split()[1] == "A"]
    if link == 0: assert absr, "absolute pointer reports over USB"
    else: assert s.host()["dx"] != h0, "relative fallback over the radio"

# ---------------- Steam Deck gamepad template ----------------
@test
@each_link
def deck_template(link, xbox):
    if skip("deck"): return
    s = sim(load("deck"), link, xbox=xbox)
    s.run(30, A=1); assert pad_btn(s, 0); s.run(30, A=0)
    s.run(30, L5=1); assert pad_btn(s, 15), "L5 = left stick click"; s.run(30, L5=0)
    s.run(30, R5=1); assert pad_btn(s, 5), "R5 = right stick click"; s.run(30, R5=0)
    s.run(40, lsx=6000); assert pad_stick(s, 0)[0] == 0, "dead zone 7199"
    s.run(40, lsx=32767); assert pad_stick(s, 0)[0] > 30000; s.run(40, lsx=0)
    s.run(40, tl=20000); assert pad_trig(s, 0) > 15000; s.run(40, tl=0)
    # left pad as D-pad: Steam's default on a trackpad needs the click
    touch(s, 0, 0, 25000, 40); assert not pad_btn(s, 13), "touch alone"
    touch(s, 0, 0, 25000, 40, click=True); assert pad_btn(s, 13), "click at the top = D-pad up"
    lift(s, 0, 40); assert not pad_btn(s, 13)

# ---------------- Deadlock: game actions with the keys the game names ----------------
@test
@each_link
def deadlock_menu_key(link, xbox):
    if skip("deadlock"): return
    r = load("deadlock")
    p = r["profiles"][0]
    src = [s2 for s2 in range(30) if 0x29 in p[488 + 14 * s2 + 1: 488 + 14 * s2 + 13]]
    assert src, "Esc (OpenMainMenu) bound somewhere"
    name = [k for k, v in BTN.items() if v == src[0]][0]
    long = 0x29 in p[488 + 14 * src[0] + 5: 488 + 14 * src[0] + 9]   # on the long press
    s = sim(r, link, xbox=xbox)
    s.log.clear(); s.run(1200 if long else 60, **{name: 1}); s.run(300, **{name: 0}); s.run(100)
    assert any(e.split()[1] == "K" and "29" in e.split()[2:] for e in s.log), (name, long, s.log[-10:])
    assert not keys(s)
    if long:   # a short press does not open the menu
        s.log.clear(); s.run(100, **{name: 1}); s.run(300, **{name: 0})
        assert not any(e.split()[1] == "K" and "29" in e.split()[2:] for e in s.log)

@test
def every_file_round_trips():
    """import -> export -> import gives the same profiles"""
    from test_vdf import roundtrip
    for name in FILES:
        path = os.path.join(DIR, FILES[name])
        if os.path.exists(path): roundtrip(open(path, encoding="utf-8").read())

@test
def every_file_every_link_random_play():
    """all sets of every file, random play on every link: nothing stays pressed afterwards, the profile is intact"""
    import random
    for name in FILES:
        r = load(name)
        if r is None: continue
        for link, nm, loss, ci in [(0, "USB", 0, 15), (1, "Puck", 0, 15), (2, "BT", 0, 15), (1, "Puck lossy", 200, 15), (2, "BT lossy, 45 ms", 150, 45)]:
            rnd = random.Random(zlib.crc32((name + nm).encode()) + SEED)
            for slot in range(len(r["profiles"])):
                if not r["profiles"][slot]: continue
                s = sim(r, link, slot=slot, loss=loss, ci=ci)
                for _ in range(250):
                    kw = {rnd.choice(list(BTN)): rnd.randint(0, 1), "lsx": rnd.choice([0, 20000, -32767]), "rsy": rnd.choice([0, 32767]),
                          "tl": rnd.choice([0, 15000, 32767]), "tr": rnd.choice([0, 32767])}
                    if rnd.random() < .3: kw.update(LP_TOUCH=1, lpx=rnd.randint(-30000, 30000), lpy=rnd.randint(-30000, 30000), LP_CLICK=rnd.randint(0, 1))
                    if rnd.random() < .3: kw.update(RP_TOUCH=1, rpx=rnd.randint(-30000, 30000), rpy=rnd.randint(-30000, 30000))
                    s.run(rnd.choice([1, 3, 8, 30]), **kw)
                s.set(**{k: 0 for k in s.state}); s.run(3000)
                h = s.host()
                assert not (set(h["kbd"][2:]) - {0}) and h["kbd"][0] == 0 and h["mb"] == 0, (name, nm, slot, h)
                assert s.out["buttons"] == 0 and s.out["tl"] == 0 and s.out["tr"] == 0, (name, nm, slot, hex(s.out["buttons"]))
                assert s.lib.sim_xbox_buttons() == 0 and pad_stick(s, 0) == (0, 0), (name, nm, slot)
                assert s.download(slot) == bytes(r["profiles"][slot])

if __name__ == "__main__":
    ok = 0
    for t in TESTS:
        try: t(); ok += 1; print("ok  ", t.__name__)
        except Exception: print("FAIL", t.__name__); traceback.print_exc()
    print(f"{ok}/{len(TESTS)} passed"); sys.exit(ok != len(TESTS))
