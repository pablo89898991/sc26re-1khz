"""Every Steam Input mode on every stick / trackpad, imported with web/vdf.js and played on remap.c:
what the mode is for actually happens (keys, mouse, wheel, sticks), and it survives export -> import."""
import sys, traceback, math
from simlib import *
from test_vdf import vdf, config, group, inp, act, roundtrip, prof_of
from test_remap import touch, lift

TESTS = []
def test(f): TESTS.append(f); return f
SRCN = {0: "joystick", 1: "right_joystick", 2: "left_trackpad", 3: "right_trackpad"}

def one(i, mode, inputs="", settings=None, extra_groups=(), **opts):
    g = group(0, mode, inputs, settings)
    gs = [g] + list(extra_groups)
    gsb = [(0, SRCN[i] + " active")] + [(k + 1, w) for k, (_, w) in enumerate([])]
    txt = config(gs, [(0, "Default", gsb)])
    r = vdf(txt, opts)
    roundtrip(txt, opts)
    return r, prof_of(r["profiles"][0])

def fresh(p):
    s = Sim(); s.use(p, 0); s.run(10); return s

def held(s): k = Events(s.log).kbd(); return set(k[-1]) if k else set()
def stick(i, x, y): return {("rsx" if i else "lsx"): x, ("rsy" if i else "lsy"): y}
def click(i): return "R3" if i else "L3"
def K(n): return key(str(n)) if isinstance(n, int) else key(n)
def binds(names): return "".join(inp(n, act(binding=f"key_press {k.upper()}, , ")) for n, k in names)

@test
def dpad_on_sticks_and_pads():
    names = [("dpad_north", "w"), ("dpad_south", "s"), ("dpad_west", "a"), ("dpad_east", "d"), ("click", "c")]
    for i in range(4):
        r, p = one(i, "dpad", binds(names))
        s = fresh(p)
        for (dx, dy), k in (((0, 30000), "w"), ((0, -30000), "s"), ((-30000, 0), "a"), ((30000, 0), "d")):
            if i < 2:
                s.run(40, **stick(i, dx, dy)); assert K(k) in held(s), (i, k, held(s)); s.run(40, **stick(i, 0, 0)); assert not held(s)
            else:
                touch(s, i - 2, dx, dy, 40); assert K(k) not in held(s), ("needs the click", i, k)
                touch(s, i - 2, dx, dy, 40, click=True); assert K(k) in held(s), (i, k, held(s)); lift(s, i - 2, 40); assert not held(s)
        if i < 2: s.run(40, **{click(i): 1}); assert K("c") in held(s); s.run(40, **{click(i): 0})
        # requires_click 0 on a pad: the touch is enough
        if i >= 2:
            r, p = one(i, "dpad", binds(names), {"requires_click": "0"}); s = fresh(p)
            touch(s, i - 2, 0, 30000, 40); assert K("w") in held(s); lift(s, i - 2, 40)

@test
def eight_way_layout():
    names = [("dpad_north", "w"), ("dpad_east", "d")]
    r, p = one(0, "dpad", binds(names), {"layout": "1"}); s = fresh(p)
    s.run(40, lsx=22000, lsy=22000); assert {K("w"), K("d")} <= held(s), held(s)
    r, p = one(0, "dpad", binds(names), {"layout": "0"}); s = fresh(p)
    s.run(40, lsx=22000, lsy=23000); assert len(held(s)) == 1, held(s)

@test
def button_pad_on_trackpads():
    names = [("button_y", "y"), ("button_a", "a"), ("button_x", "x"), ("button_b", "b")]
    for i in (2, 3):
        r, p = one(i, "four_buttons", binds(names)); s = fresh(p)
        for (dx, dy), k in (((0, 30000), "y"), ((0, -30000), "a"), ((-30000, 0), "x"), ((30000, 0), "b")):
            touch(s, i - 2, dx, dy, 40, click=True); assert K(k) in held(s), (i, k, held(s)); lift(s, i - 2, 40)

@test
def mouse_modes_move_the_mouse():
    for i, mode in ((0, "joystick_mouse"), (1, "joystick_mouse"), (0, "joystick_camera"), (1, "flickstick")):
        r, p = one(i, mode); s = fresh(p); h0 = s.host()["dx"]
        s.run(200, **stick(i, 30000, 0)); s.run(100, **stick(i, 0, 0))
        assert s.host()["dx"] - h0 > 30, (i, mode, s.host()["dx"] - h0)
    for i in (2, 3):
        for st in ({}, {"trackball": "0"}, {"trackball": "1", "friction": "2"}):
            r, p = one(i, "absolute_mouse", settings=st); s = fresh(p); h0 = s.host()["dx"]
            touch(s, i - 2, 0, 0, 20)
            for k in range(20): touch(s, i - 2, k * 600, 0, 4)
            lift(s, i - 2, 200)
            assert s.host()["dx"] - h0 > 20, (i, st, s.host()["dx"] - h0)

@test
def joystick_move_and_mouse_joystick_are_sticks():
    for i, mode in ((0, "joystick_move"), (1, "joystick_move"), (2, "mouse_joystick"), (3, "mouse_joystick"), (2, "joystick_move")):
        r, p = one(i, mode); s = fresh(p)
        if i < 2: s.run(40, **stick(i, 30000, 0))
        else: touch(s, i - 2, 30000, 0, 40)
        side = i % 2
        x = s.lib.sim_xbox_stick(side, 0)
        assert x > 20000, (i, mode, x)
        s.run(40, **stick(i, 0, 0)) if i < 2 else lift(s, i - 2, 40)
        assert s.lib.sim_xbox_stick(side, 0) == 0
        # no Xbox pad (Bluetooth): the Steam pad's stick
        s = Sim(); s.transport(2); s.use(p, 0); s.run(10)
        if i < 2: s.run(40, **stick(i, 30000, 0))
        else: touch(s, i - 2, 30000, 0, 40)
        assert s.out["rsx" if side else "lsx"] > 20000, (i, mode, s.out)

@test
def scroll_wheel():
    r, p = one(0, "scrollwheel"); s = fresh(p); w0 = s.host()["wh"]
    s.run(300, lsy=30000); s.run(50, lsy=0); assert s.host()["wh"] != w0, "stick scroll"
    names = [("scroll_clockwise", "e"), ("scroll_counterclockwise", "q")]
    for i in (2, 3):
        r, p = one(i, "scrollwheel", binds(names)); s = fresh(p); s.log.clear()
        for k in range(80):
            a = k * 0.12
            touch(s, i - 2, int(25000 * math.sin(a)), int(25000 * math.cos(a)), 4)
        lift(s, i - 2, 50)
        seen = set().union(*[set(x) for x in Events(s.log).kbd()]) if Events(s.log).kbd() else set()
        assert K("e") in seen and K("q") not in seen, (i, seen)

@test
def radial_menus():
    for i in range(4):
        n = 8 if i < 2 else 6
        ins = "".join(inp(f"touch_menu_button_{k}", act(binding=f"key_press {k + 1}, , ")) for k in range(n))
        for mode in ("radial_menu", "touch_menu"):
            r, p = one(i, mode, ins); s = fresh(p); s.log.clear()
            # slice 2: right (8 slices) / 120 degrees (6 slices)
            ang = 90 if n == 8 else 120
            x, y = int(28000 * math.sin(math.radians(ang))), int(28000 * math.cos(math.radians(ang)))
            if i < 2: s.run(60, **stick(i, x, y)); s.run(60, **stick(i, 0, 0))
            else: touch(s, i - 2, x, y, 60); lift(s, i - 2, 60)
            seen = set().union(*[set(x) for x in Events(s.log).kbd()]) if Events(s.log).kbd() else set()
            assert seen == {K(3)}, (i, mode, seen)

@test
def single_button():
    for i in range(4):
        r, p = one(i, "single_button", binds([("click", "g")])); s = fresh(p)
        if i < 2:
            s.run(40, **stick(i, 0, 30000)); assert K("g") in held(s), (i, held(s)); s.run(40, **stick(i, 0, 0)); assert not held(s)
        else:
            touch(s, i - 2, 5000, 5000, 40, click=True); assert K("g") in held(s), (i, held(s)); lift(s, i - 2, 40); assert not held(s)

@test
def touch_and_click_inputs_of_trackpads():
    for i in (2, 3):
        r, p = one(i, "absolute_mouse", binds([("touch", "t"), ("click", "k")])); s = fresh(p)
        touch(s, i - 2, 0, 0, 40); assert K("t") in held(s) and K("k") not in held(s), held(s)
        touch(s, i - 2, 0, 0, 40, click=True); assert {K("t"), K("k")} <= held(s); lift(s, i - 2, 40); assert not held(s)

@test
def outer_ring():
    r, p = one(0, "joystick_move", binds([("edge", "r")])); s = fresh(p)
    s.run(40, lsx=15000); assert K("r") not in held(s)
    s.run(40, lsx=32000); assert K("r") in held(s); s.run(40, lsx=0)

@test
def unknown_mode_is_reported():
    r, p = one(1, "made_up_mode")
    assert any("not available" in n for n in r["notes"]), r["notes"]

@test
def inverted_and_rotated():
    r, p = one(0, "joystick_mouse", settings={"invert_x": "1"}); s = fresh(p); h0 = s.host()["dx"]
    s.run(200, lsx=30000); assert s.host()["dx"] < h0, "inverted"

@test
def mode_shift_of_a_stick_and_a_trackpad():
    base = group(0, "joystick_move")
    shifted = group(1, "joystick_mouse", settings={"sensitivity": "200"})
    padb = group(2, "dpad", binds([("dpad_north", "w")]), {"requires_click": "0"})
    pads = group(3, "dpad", binds([("dpad_north", "u")]), {"requires_click": "0"})
    sw = group(4, "switches", inp("button_back_left", act(binding="mode_shift right_joystick 1"), act(binding="mode_shift left_trackpad 3")))
    txt = config([base, shifted, padb, pads, sw], [(0, "Default", [(0, "right_joystick active"), (1, "right_joystick inactive modeshift"),
                                                                   (2, "left_trackpad active"), (3, "left_trackpad inactive modeshift"), (4, "switch active")])])
    r = vdf(txt); assert not r["notes"] or all("Xbox 360" in n for n in r["notes"]), r["notes"]
    roundtrip(txt)
    p = prof_of(r["profiles"][0]); s = fresh(p)
    h0 = s.host()["dx"]; s.run(100, rsx=30000); assert s.host()["dx"] == h0 and s.lib.sim_xbox_stick(1, 0) > 20000
    s.run(30, rsx=0, L5=1); h0 = s.host()["dx"]; s.run(200, rsx=30000); assert s.host()["dx"] - h0 > 30, "shifted: mouse"
    s.run(30, rsx=0)
    touch(s, 0, 0, 30000, 40); assert K("u") in held(s), held(s); lift(s, 0, 40)
    s.run(30, L5=0); touch(s, 0, 0, 30000, 40); assert K("w") in held(s); lift(s, 0, 40)

@test
def activators_on_derived_inputs():
    names = inp("dpad_north", act(binding="key_press W, , "), act("Long_Press", "key_press L, , ", {"long_press_time": "300"})) + \
            inp("dpad_south", act("Double_Press", "key_press D, , ", {"double_tap_time": "250"})) + \
            inp("dpad_east", act(binding="key_press T, , ", settings={"toggle": "1"}))
    r, p = one(0, "dpad", names); s = fresh(p)
    s.run(100, lsy=30000); assert K("w") in held(s) and K("l") not in held(s)
    s.run(300); assert K("l") in held(s), held(s); s.run(50, lsy=0); s.run(50)
    s.log.clear(); s.run(40, lsy=-30000); s.run(40, lsy=0); s.run(40, lsy=-30000); s.run(80, lsy=0)
    seen = set().union(*[set(x) for x in Events(s.log).kbd()]); assert K("d") in seen, seen
    s.run(300); s.run(40, lsx=30000); s.run(40, lsx=0); assert K("t") in held(s); s.run(40, lsx=30000); s.run(40, lsx=0); assert K("t") not in held(s)

@test
def action_sets_switch_on_every_link():
    a = group(0, "four_buttons", inp("button_a", act(binding="key_press A, , ")) + inp("button_y", act(binding="controller_action CHANGE_PRESET 2 0 0, , ")))
    b = group(1, "four_buttons", inp("button_a", act(binding="key_press B, , ")) + inp("button_y", act(binding="controller_action CHANGE_PRESET 1 0 0, , ")))
    js = group(2, "joystick_mouse")
    txt = config([a, b, js], [(0, "Default", [(0, "button_diamond active")]), (1, "Second", [(1, "button_diamond active"), (2, "joystick active")])])
    r = vdf(txt); roundtrip(txt)
    for link in (0, 1, 2):
        s = Sim(); s.transport(link, 1 if link == 1 else 0)
        for k, x in enumerate(r["profiles"]):
            if x: s.upload(k, prof_of(x))
        s.cfg("remap_profile", 0); s.cfg("remap_on", 1); s.run(30)
        def hk(): return set(s.host()["kbd"][2:]) - {0}
        s.run(60, A=1); assert key("a") in hk(), (link, hk()); s.run(60, A=0); s.run(60)
        s.run(60, Y=1); s.run(60, Y=0); s.run(60)
        s.run(60, A=1); assert key("b") in hk(), (link, hk()); s.run(60, A=0); s.run(60)
        h0 = s.host()["dx"]; s.run(200, lsx=30000); s.run(100, lsx=0); assert s.host()["dx"] - h0 > 30, "the second set's stick"
        s.run(60, Y=1); s.run(60, Y=0); s.run(60)
        s.run(60, A=1); assert key("a") in hk(); s.run(60, A=0); s.run(100); assert not hk()

@test
def playstation_touchpad_goes_to_the_right_trackpad():
    g = group(0, "absolute_mouse", binds([("click", "m")]))
    txt = config([g], [(0, "Default", [(0, "center_trackpad active")])]).replace('"controller_triton"', '"controller_ps5"')
    r = vdf(txt); p = prof_of(r["profiles"][0])
    assert p.b[256 + 18 * 3] == 3 and any("right trackpad" in n for n in r["notes"]), (p.b[256 + 18 * 3], r["notes"])
    s = fresh(p); touch(s, 1, 0, 0, 40, click=True); assert K("m") in held(s); lift(s, 1, 40)

if __name__ == "__main__":
    ok = 0
    for t in TESTS:
        try: t(); ok += 1; print("ok  ", t.__name__)
        except Exception: print("FAIL", t.__name__); traceback.print_exc()
    print(f"{ok}/{len(TESTS)} passed"); sys.exit(ok != len(TESTS))
