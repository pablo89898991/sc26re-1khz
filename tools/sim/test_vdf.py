"""Steam Input .vdf import / export (web/vdf.js) driven against the real remap.c."""
import sys, json, subprocess, traceback, os
from simlib import *
from test_remap import touch, lift

UP = os.environ.get("VDF_SAMPLE", "/mnt/user-data/uploads/0289a2b4-3737914488_controller_config.vdf")
TESTS = []
def test(f): TESTS.append(f); return f

def vdf(text=None, opts=None, export=None, exportOpts=None):
    q = dict(text=text, opts=opts or {})
    if export is not None: q.update(export=[list(p) if p else None for p in export], exportOpts=exportOpts or {})
    r = subprocess.run(["node", os.path.join(HERE, "vdf_cli.js")], input=json.dumps(q).encode(), capture_output=True)
    assert r.returncode == 0, r.stderr.decode()
    return json.loads(r.stdout)

def prof_of(lst):
    p = Profile(); p.b[:] = bytes(lst); return p

def held(s): k = Events(s.log).kbd(); return k[-1] if k else ()

# ---- a small config written the way Steam writes them ----
def act(kind="Full_Press", binding="key_press SPACE, , ", settings=None):
    st = "".join(f'\t\t\t\t\t\t\t"{k}"\t\t"{v}"\n' for k, v in (settings or {}).items())
    return f'''\t\t\t\t\t"{kind}"
\t\t\t\t\t{{
\t\t\t\t\t\t"bindings"
\t\t\t\t\t\t{{
\t\t\t\t\t\t\t"binding"\t\t"{binding}"
\t\t\t\t\t\t}}
''' + (f'\t\t\t\t\t\t"settings"\n\t\t\t\t\t\t{{\n{st}\t\t\t\t\t\t}}\n' if st else "") + "\t\t\t\t\t}\n"

def inp(name, *acts):
    return f'\t\t\t"{name}"\n\t\t\t{{\n\t\t\t\t"activators"\n\t\t\t\t{{\n' + "".join(acts) + '\t\t\t\t}\n\t\t\t\t"disabled_activators"\n\t\t\t\t{\n\t\t\t\t}\n\t\t\t}\n'

def group(gid, mode, inputs="", settings=None):
    st = "".join(f'\t\t\t"{k}"\t\t"{v}"\n' for k, v in (settings or {}).items())
    return f'\t"group"\n\t{{\n\t\t"id"\t\t"{gid}"\n\t\t"mode"\t\t"{mode}"\n\t\t"inputs"\n\t\t{{\n{inputs}\t\t}}\n' + \
        (f'\t\t"settings"\n\t\t{{\n{st}\t\t}}\n' if st else "") + "\t}\n"

def config(groups, presets, layers=""):
    pr = ""
    for pid, name, gsb in presets:
        pr += f'\t"preset"\n\t{{\n\t\t"id"\t\t"{pid}"\n\t\t"name"\t\t"{name}"\n\t\t"group_source_bindings"\n\t\t{{\n' + \
            "".join(f'\t\t\t"{g}"\t\t"{w}"\n' for g, w in gsb) + "\t\t}\n\t}\n"
    acts = "".join(f'\t\t"{n}"\n\t\t{{\n\t\t\t"title"\t\t"{n}"\n\t\t\t"legacy_set"\t\t"1"\n\t\t}}\n' for _, n, _ in presets if not n.startswith("L_"))
    lay = "".join(f'\t\t"{n}"\n\t\t{{\n\t\t\t"title"\t\t"{n}"\n\t\t\t"legacy_set"\t\t"1"\n\t\t\t"set_layer"\t\t"1"\n\t\t\t"parent_set_name"\t\t"Default"\n\t\t}}\n' for _, n, _ in presets if n.startswith("L_"))
    return f'"controller_mappings"\n{{\n\t"version"\t\t"3"\n\t"controller_type"\t\t"controller_triton"\n\t"actions"\n\t{{\n{acts}\t}}\n' + \
        (f'\t"action_layers"\n\t{{\n{lay}\t}}\n' if lay else "") + "".join(groups) + pr + "}\n"

def one(groups, gsb, **opts):
    r = vdf(config(groups, [(0, "Default", gsb)]), opts)
    return r, prof_of(r["profiles"][0])

def fresh(p, slot=0):
    s = Sim(); s.use(p, slot); s.run(5); return s

@test
def keys_and_mouse_buttons():
    g = group(0, "four_buttons", inp("button_a", act(binding="key_press SPACE, Jump, , ")) + inp("button_b", act(binding="mouse_button RIGHT, , ")) +
              inp("button_x", act(binding="key_press LEFT_SHIFT, , ")) + inp("button_y", act(binding="key_press F5, , ")))
    sw = group(1, "switches", inp("button_back_left", act(binding="key_press E, , ")) + inp("left_bumper", act(binding="mouse_wheel SCROLL_UP, , ")))
    r, p = one([g, sw], [(0, "button_diamond active"), (1, "switch active")])
    assert not r["notes"], r["notes"]
    s = fresh(p)
    s.run(30, A=1); assert key("space") in held(s); s.run(30, A=0); assert key("space") not in held(s)
    s.run(30, X=1); assert 0xE1 in held(s); s.run(30, X=0)
    s.run(30, Y=1); assert 0x3E in held(s); s.run(30, Y=0)
    s.run(30, L5=1); assert key("e") in held(s); s.run(30, L5=0)
    e = s.run(30, B=1); assert any(m[0] & 2 for m in e.mouse()), e.mouse()
    s.run(30, B=0)

@test
def long_double_start_release_toggle_turbo():
    g = group(0, "four_buttons",
              inp("button_a", act(binding="key_press A, , "), act("Long_Press", "key_press B, , ", {"long_press_time": "400"})) +
              inp("button_b", act("Double_Press", "key_press C, , ", {"double_tap_time": "300"})) +
              inp("button_x", act(binding="key_press D, , ", settings={"toggle": "1"})) +
              inp("button_y", act(binding="key_press F, , ", settings={"hold_repeats": "1", "repeat_rate": "50"})))
    r, p = one([g], [(0, "button_diamond active")])
    assert p.b[Profile.BT + 6 * 0] == 40 and p.b[Profile.BT + 6 * 1 + 1] == 30, list(p.b[Profile.BT:Profile.BT + 12])
    s = fresh(p)
    s.run(300, A=1); assert key("b") not in held(s)
    s.run(200); assert key("b") in held(s), held(s)
    s.run(50, A=0)
    s.run(20, B=1); s.run(40, B=0); s.run(20, B=1)
    seen = False
    for _ in range(40):
        s.run(5)
        if key("c") in held(s): seen = True
    assert seen, "double press"
    s.run(50, B=0)
    s.run(30, X=1); s.run(30, X=0); assert key("d") in held(s); s.run(30, X=1); s.run(30, X=0); assert key("d") not in held(s)
    s.log.clear(); s.run(500, Y=1); n = Events(s.log).taps(key("f")); s.run(30, Y=0)
    assert 6 <= n <= 14, n

@test
def delay_start_and_interruptible():
    g = group(0, "four_buttons", inp("button_a", act(binding="key_press G, , ", settings={"delay_start": "100"})) +
              inp("button_b", act(binding="key_press H, , "), act("Long_Press", "key_press I, , ", {"long_press_time": "300", "interruptable": "1"})))
    r, p = one([g], [(0, "button_diamond active")])
    s = fresh(p)
    s.run(60, A=1); assert key("g") not in held(s); s.run(80); assert key("g") in held(s); s.run(150, A=0)
    # Steam: a regular press next to a long press acts at once (not "interruptible")
    assert p.b[Profile.BT + 6 * 1 + 5] & 2
    s.run(40, B=1); assert key("h") in held(s); s.run(300); assert {key("h"), key("i")} <= set(held(s)), held(s)
    s.run(40, B=0); s.run(40); assert held(s) == ()

@test
def regular_press_with_double_press_acts_at_once():
    g = group(0, "four_buttons", inp("button_x", act(binding="key_press J, , "), act("Double_Press", "key_press K, , ", {"double_tap_time": "250"})))
    r, p = one([g], [(0, "button_diamond active")])
    s = fresh(p); s.log.clear()
    s.run(30, X=1); assert key("j") in held(s), "at once"
    s.run(30, X=0); s.run(400)
    taps = Events(s.log).taps(key("j")); assert taps == 1, ("one press, no extra tap after the window", taps)
    s.log.clear(); s.run(30, X=1); s.run(30, X=0); s.run(30, X=1)
    assert {key("j"), key("k")} <= set(held(s)), held(s)
    s.run(30, X=0); s.run(300); assert held(s) == ()
    roundtrip(config([g], [(0, "Default", [(0, "button_diamond active")])]))

@test
def double_and_release_on_one_input_keep_the_double():
    for order in (0, 1):
        a1 = act("Double_Press", "key_press D, , "); a2 = act("release", "key_press R, , ")
        g = group(0, "four_buttons", inp("button_a", *((a1, a2) if order == 0 else (a2, a1))))
        r, p = one([g], [(0, "button_diamond active")])
        assert p.b[488] & 2 and not p.b[488] & 16 and list(p.b[488 + 9:488 + 13]) == [key("d"), 0, 0, 0], (order, list(p.b[488:502]))
        assert any("double press and release" in n for n in r["notes"])

@test
def interruptible_regular_press_waits_for_the_long_press():
    g = group(0, "four_buttons", inp("button_b", act(binding="key_press H, , ", settings={"interruptable": "1"}), act("Long_Press", "key_press I, , ", {"long_press_time": "300"})))
    r, p = one([g], [(0, "button_diamond active")])
    assert not p.b[Profile.BT + 6 * 1 + 5] & 2
    s = fresh(p); s.log.clear()
    s.run(100, B=1); assert key("h") not in held(s), "waits"
    s.run(40, B=0); assert key("h") in set().union(*map(set, Events(s.log).kbd())), "short tap: the press when let go"
    s.run(100); s.log.clear(); s.run(400, B=1); assert held(s) == (key("i"),), held(s); s.run(40, B=0)
    assert key("h") not in set().union(*map(set, Events(s.log).kbd())), "long: no short press"
    roundtrip(config([g], [(0, "Default", [(0, "button_diamond active")])]))

@test
def mode_shift_on_back_button():
    base = group(0, "four_buttons", inp("button_a", act(binding="key_press J, , ")))
    shifted = group(1, "four_buttons", inp("button_a", act(binding="key_press K, , ")))
    sw = group(2, "switches", inp("button_back_right", act(binding="mode_shift button_diamond 1")))
    r, p = one([base, shifted, sw], [(0, "button_diamond active"), (1, "button_diamond inactive modeshift"), (2, "switch active")])
    assert not r["notes"], r["notes"]
    s = fresh(p)
    s.run(30, A=1); assert key("j") in held(s); s.run(30, A=0)
    s.run(30, R5=1); s.run(30, A=1); assert key("k") in held(s) and key("j") not in held(s), held(s); s.run(30, A=0); s.run(30, R5=0)
    s.run(30, A=1); assert key("j") in held(s); s.run(30, A=0)

@test
def stick_dpad_and_click():
    g = group(0, "dpad", inp("dpad_north", act(binding="key_press W, , ")) + inp("dpad_south", act(binding="key_press S, , ")) +
              inp("dpad_west", act(binding="key_press A, , ")) + inp("dpad_east", act(binding="key_press D, , ")) +
              inp("click", act(binding="key_press LEFT_CONTROL, , ")))
    r, p = one([g], [(0, "joystick active")])
    s = fresh(p)
    s.run(40, lsy=30000); assert key("w") in held(s), held(s); s.run(40, lsy=0)
    s.run(40, lsx=-30000); assert key("a") in held(s); s.run(40, lsx=0)
    s.run(40, L3=1); assert 0xE0 in held(s); s.run(40, L3=0)

@test
def joystick_mouse_on_stick_and_trackpad_mouse():
    st = group(0, "joystick_mouse", settings={"sensitivity": "200", "sensitivity_vert_scale": "50"})
    tp = group(1, "absolute_mouse", inp("click", act(binding="mouse_button LEFT, , ")), {"sensitivity": "150", "trackball": "0"})
    r, p = one([st, tp], [(0, "right_joystick active"), (1, "right_trackpad active")])
    s = fresh(p)
    e = s.run(100, rsx=30000); dx = sum(m[1] for m in e.mouse()); assert dx > 50, dx
    s.run(50, rsx=0)
    touch(s, 1, 0, 0, 10); e = Events()
    for k in range(20): e += touch(s, 1, k * 600, 0, 5)
    dx = sum(m[1] for m in e.mouse()); assert dx > 20, dx
    lift(s, 1)
    assert int.from_bytes(p.b[2968 + 12 * 1 + 2:2968 + 12 * 1 + 4], "little") == 50

@test
def triggers_soft_full_and_xbox_analog():
    lt = group(0, "trigger", inp("click", act(binding="mouse_button RIGHT, , ")) + inp("edge", act(binding="key_press Q, , ")), {"output_trigger": "1"})
    r, p = one([lt], [(0, "left_trigger active")])
    assert XTRIG_L in p.b[488 + 14 * 58: 488 + 14 * 59], list(p.b[488 + 14 * 58: 488 + 14 * 59])
    s = fresh(p)
    e = s.run(40, tl=26000); assert any(m[0] & 2 for m in e.mouse()); assert key("q") not in held(s)
    s.run(40, tl=32767, LT_CLICK=1); assert key("q") in held(s)
    s.run(40, tl=0, LT_CLICK=0)

@test
def action_sets_and_layers():
    a = group(0, "four_buttons", inp("button_a", act(binding="key_press L, , ")) + inp("button_y", act(binding="controller_action CHANGE_PRESET 2 0 0, , ")))
    sw = group(1, "switches", inp("button_back_left", act(binding="controller_action hold_layer 3 0 0, , ")))
    b = group(2, "four_buttons", inp("button_a", act(binding="key_press M, , ")))
    lay = group(3, "four_buttons", inp("button_a", act(binding="key_press N, , ")))
    txt = config([a, sw, b, lay], [(0, "Default", [(0, "button_diamond active"), (1, "switch active")]), (1, "Second", [(2, "button_diamond active")]),
                                   (2, "L_hold", [(3, "button_diamond active")])])
    r = vdf(txt)
    assert r["layers"] == [False, True, False, False] or r["layers"][:3] == [False, True, False], r["layers"]
    # default order: set 1, its layer, set 2
    P = [prof_of(x) if x else None for x in r["profiles"]]
    s = Sim()
    for k, q in enumerate(P):
        if q: s.upload(k, q)
    s.cfg("remap_profile", 0); s.cfg("remap_on", 1); s.run(5)
    s.run(30, A=1); assert key("l") in held(s); s.run(30, A=0)
    s.run(30, L5=1); s.run(30, A=1); assert key("n") in held(s), held(s); s.run(30, A=0); s.run(30, L5=0)
    s.run(30, A=1); assert key("l") in held(s); s.run(30, A=0)
    s.run(30, Y=1); s.run(30, Y=0); s.run(20)
    s.run(30, A=1); assert key("m") in held(s), held(s); s.run(30, A=0)

@test
def radial_menu_on_stick():
    ins = "".join(inp(f"touch_menu_button_{k}", act(binding=f"key_press {k + 1}, , ")) for k in range(4))
    g = group(0, "radial_menu", ins)
    r, p = one([g], [(0, "right_joystick active")])
    s = fresh(p)
    s.run(60, rsx=30000); s.run(5, rsx=0)
    seen = set()
    for _ in range(40):
        s.run(2)
        seen |= set(held(s))
    assert key("2") in seen, seen   # 4 slices: up, right, down, left

@test
def gyro_to_mouse_with_button():
    gy = group(0, "gyro_to_mouse", settings={"gyro_natural_sensitivity": "250", "gyro_button": "right_bumper", "gyro_button_invert": "0", "gyro_to_2d_conversion_style": "4"})
    r, p = one([gy], [(0, "gyro active")])
    g = lambda k: struct.unpack_from("<h", p.b, 44 + 2 * k)[0]
    assert p.b[17] & 6 == 6 and g(6) == 250 and g(1) == 9 and g(2) == 1, (p.b[17], g(6), g(1), g(2))
    assert p.b[Profile.GYX + 1] == 2

@test
def game_actions_map_with_keys():
    g = group(0, "four_buttons", inp("button_a", act(binding="game_action Default Jump, Jump, , ")))
    r, p = one([g], [(0, "button_diamond active")], gameMap={"Jump": key("space")})
    s = fresh(p); s.run(30, A=1); assert key("space") in held(s)
    r, p = one([g], [(0, "button_diamond active")])
    assert any("game action" in n for n in r["notes"]), r["notes"]
    assert p.b[488] == 0 and p.b[489] == 0

@test
def unbound_activators_leave_nothing():
    g = group(0, "four_buttons", inp("button_a", act("Long_Press", "game_action Default X, , ", {"long_press_time": "800"})))
    r, p = one([g], [(0, "button_diamond active")])
    assert p.b[488] == 0 and p.b[Profile.BT] == 0

def roundtrip(text, opts=None):
    r = vdf(text, opts)
    e = vdf(export=[bytes(x) if x else None for x in r["profiles"]], exportOpts={"layers": r["layers"]})
    r2 = vdf(e["text"], opts)
    for k, (a, b) in enumerate(zip(r["profiles"], r2["profiles"])):
        if a is None: continue
        d = [i for i in range(len(a)) if a[i] != b[i]]
        assert not d, (k, d[:10], [(a[i], b[i]) for i in d[:10]])
    return r, e

@test
def user_file_imports_and_round_trips():
    if not os.path.exists(UP): print("     (no sample .vdf at %s: skipped)" % UP); return
    txt = open(UP, encoding="utf-8").read()
    r, e = roundtrip(txt)
    assert len(r["list"]) == 5 and r["layers"][1] is True, (r["list"], r["layers"])
    assert len(r["game"]) > 50 and any(a["name"] == "OpenMainMenu" and a["def"] == key("esc") for a in r["game"])
    # all four run on the firmware without trouble
    s = Sim()
    for k, x in enumerate(r["profiles"]): s.upload(k, prof_of(x))
    s.cfg("remap_on", 1)
    import random; rnd = random.Random(5)
    for k in range(4):
        s.cfg("remap_profile", k); s.run(5)
        for _ in range(300):
            s.run(3, **{rnd.choice(list(BTN)): rnd.randint(0, 1)}, lsx=rnd.randint(-32767, 32767), rsy=rnd.randint(-32767, 32767), tl=rnd.randint(0, 32767))
        assert s.download(k) == bytes(r["profiles"][k])

@test
def roundtrip_of_rich_config():
    ins = "".join(inp(f"touch_menu_button_{k}", act(binding=f"key_press {k + 1}, , ")) for k in range(6))
    G = [group(0, "four_buttons", inp("button_a", act(binding="key_press A, , "), act("Long_Press", "key_press B, , ", {"long_press_time": "500", "interruptable": "1"})) +
               inp("button_b", act("Double_Press", "mouse_button MIDDLE, , ", {"double_tap_time": "250"})) + inp("button_y", act("Start_Press", "key_press TAB, , ", {"delay_end": "120", "haptic_intensity": "2"}))),
         group(1, "radial_menu", ins), group(2, "absolute_mouse", inp("click", act("Soft_Press", "mouse_button LEFT, , ")), {"sensitivity": "175", "trackball": "1", "friction": "3", "edge_spin_velocity": "40"}),
         group(3, "mouse_region", inp("click", act(binding="mouse_button LEFT, , ")), {"position_x": "30", "position_y": "60", "scale": "40"}),
         group(4, "trigger", inp("edge", act(binding="key_press R, , ")), {"output_trigger": "2"}),
         group(5, "switches", inp("button_back_right", act(binding="mode_shift button_diamond 6")) + inp("button_menu", act(binding="key_press ESCAPE, , ")) +
               inp("always_on_action", act(binding="controller_action set_led 255 0 128 100 100 1, , "))),
         group(6, "four_buttons", inp("button_a", act(binding="key_press Z, , "))),
         group(7, "gyro_to_mouse", settings={"gyro_natural_sensitivity": "320", "gyro_button": "left_bumper", "gyro_button_invert": "1", "gyro_to_2d_conversion_style": "6", "acceleration": "2"}),
         group(8, "dpad", inp("dpad_north", act(binding="xinput_button A, , ")), {"layout": "1"})]
    txt = config(G, [(0, "Default", [(0, "button_diamond active"), (1, "right_joystick active"), (2, "right_trackpad active"), (3, "left_trackpad active"),
                                      (4, "right_trigger active"), (5, "switch active"), (6, "button_diamond inactive modeshift"), (7, "gyro active"), (8, "dpad active")])])
    r, e = roundtrip(txt)
    assert all("Xbox 360 pad" in n or "3D to 2D" in n or "soft press" in n for n in r["notes"]), r["notes"]
    assert not e["dropped"], e["dropped"]

@test
def chorded_press_both_ways():
    g = group(0, "four_buttons", inp("button_a", act("Chorded_Press", "key_press T, , ", {"chord_button": "left_bumper"})))
    r, p = one([g], [(0, "button_diamond active")])
    s = fresh(p)
    s.run(30, A=1); assert key("t") not in held(s); s.run(30, A=0)
    s.run(30, LB=1); s.run(30, A=1); assert key("t") in held(s); s.run(30, A=0, LB=0)
    roundtrip(config([g], [(0, "Default", [(0, "button_diamond active")])]))

if __name__ == "__main__":
    ok = 0
    for t in TESTS:
        try: t(); ok += 1; print("ok  ", t.__name__)
        except Exception: print("FAIL", t.__name__); traceback.print_exc()
    print(f"{ok}/{len(TESTS)} passed"); sys.exit(ok != len(TESTS))
