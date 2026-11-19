"""The user's own remapper profiles (v6 JSON exports): upgraded by the firmware and by the page,
then played on every link: every plain button binding gives its keys / mouse buttons / pad
buttons, nothing stays pressed."""
import os, sys, json, random, traceback, subprocess
from simlib import *

DIR = os.environ.get("PROFILE_DIR", "/mnt/user-data/uploads")
FILES = [f for f in sorted(os.listdir(DIR)) if f.endswith(".json") and "sc2-remap" in f] if os.path.isdir(DIR) else []
TESTS = []
def test(f): TESTS.append(f); return f
def data(f): return bytes(json.load(open(os.path.join(DIR, f)))["data"])

@test
def firmware_upgrades_v6_uploads():
    for f in FILES:
        d = data(f); assert len(d) == 2056 and d[1] == 6
        s = Sim()
        for o in range(0, len(d), 40):
            assert s.feature(0xEE, "sc26re/rmp/2/%x" % o, d[o:o + 40])[0] == 0
        assert s.feature(0xEF, "sc26re/rmp/2")[0] == 0
        b = s.download(2)
        assert b[0] == 0xA7 and b[1] == 7 and b[2:2056] == d[2:2056] and b[2056:] == bytes(3144 - 2056), f

@test
def page_upgrade_matches_the_firmware():
    js = "const fs=require('fs');const src=fs.readFileSync('../../web/remap.html','utf8');" \
         "const code=src.match(/const PROF_SIZE=3144[^\\n]*\\n/)[0]+src.match(/function upgradeProfile[^\\n]*\\n/)[0];" \
         "const d=JSON.parse(fs.readFileSync(process.argv[1])).data;" \
         "process.stdout.write(JSON.stringify(Array.from(eval(code+';upgradeProfile(new Uint8Array(d))'))))"
    for f in FILES:
        r = subprocess.run(["node", "-e", js, os.path.join(DIR, f)], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        b = bytes(json.loads(r.stdout)); d = data(f)
        assert len(b) == 3144 and b[1] == 7 and b[2:2056] == d[2:2056] and b[2056:] == bytes(1088), f

def expected(p, s):
    """what a plain binding (no press modes) of button s does: keys, mouse buttons, pad bits"""
    o = 488 + 14 * s
    if p[o] != 0 or (p[o + 13] >> 2): return None
    acts = [a for a in p[o + 1:o + 5] if a]
    if not acts or any(not (0x04 <= a <= 0x65 or 0xE0 <= a <= 0xE7 or a in (0x70, 0x71, 0x7A, 0x7B, 0x7C) or 0x80 <= a < 0x9E or 0xA0 <= a < 0xBE) for a in acts): return None
    return acts

@test
def plain_buttons_on_every_link():
    names = {v: k for k, v in BTN.items()}
    for f in FILES:
        prof = Profile(); prof.b[:] = data(f) + bytes(1088); prof.b[1] = 7
        for link in (0, 1, 2):
            s = Sim(); s.transport(link, 1 if link == 1 else 0); s.use(prof, 0); s.run(50)
            for bit in range(30):
                if bit in (16,): continue                         # Steam: profile chords / power
                acts = expected(prof.b, bit)
                if acts is None: continue
                # a shift button of some group changes what other buttons do: press them alone only
                s.run(60, **{names[bit]: 1})
                h = s.host(); keys = set(h["kbd"][2:]) - {0}; mods = h["kbd"][0]
                for a in acts:
                    if 0x04 <= a <= 0x65: assert a in keys, (f, link, names[bit], hex(a), keys)
                    elif 0xE0 <= a <= 0xE7: assert mods & (1 << (a - 0xE0)), (f, link, names[bit], hex(a))
                    elif a in (0x70, 0x71, 0x7A, 0x7B, 0x7C): assert h["mb"] & {0x70: 1, 0x71: 2, 0x7A: 4, 0x7B: 8, 0x7C: 16}[a], (f, link, names[bit], hex(a), h["mb"])
                    elif a >= 0xA0:   # Xbox pad: over USB the Xbox pad, elsewhere the Steam pad
                        if s.lib.sim_xbox_live(): assert s.lib.sim_xbox_buttons() & (1 << (a - 0xA0)), (f, link, names[bit], hex(a))
                        else: assert s.out["buttons"] & (1 << (a - 0xA0)), (f, link, names[bit], hex(a), hex(s.out["buttons"]))
                    else: assert s.out["buttons"] & (1 << (a - 0x80)), (f, link, names[bit], hex(a), hex(s.out["buttons"]))
                s.run(150, **{names[bit]: 0})
                h = s.host(); assert not (set(h["kbd"][2:]) - {0}) and h["kbd"][0] == 0 and h["mb"] == 0, (f, link, names[bit], h)

@test
def random_play_on_every_link():
    for f in FILES:
        prof = Profile(); prof.b[:] = data(f) + bytes(1088); prof.b[1] = 7
        for link, loss, ci in ((0, 0, 15), (1, 0, 15), (2, 0, 15), (1, 150, 15), (2, 100, 45)):
            rnd = random.Random(link * 7 + loss)
            s = Sim(); s.transport(link, 1 if link == 1 else 0, loss=loss, ci=ci); s.use(prof, 0); s.run(20)
            for _ in range(400):
                kw = {rnd.choice(list(BTN)): rnd.randint(0, 1), "lsx": rnd.choice([0, 20000, -32767]), "rsy": rnd.choice([0, 32767]), "tl": rnd.choice([0, 15000, 32767]), "tr": rnd.choice([0, 32767])}
                if rnd.random() < .3: kw.update(LP_TOUCH=1, lpx=rnd.randint(-30000, 30000), lpy=rnd.randint(-30000, 30000), LP_CLICK=rnd.randint(0, 1))
                if rnd.random() < .3: kw.update(RP_TOUCH=1, rpx=rnd.randint(-30000, 30000), rpy=rnd.randint(-30000, 30000), RP_CLICK=rnd.randint(0, 1))
                if kw.get("STEAM"): kw["STEAM"] = 0
                s.run(rnd.choice([1, 3, 8, 30]), **kw)
            s.set(**{k: 0 for k in s.state}); s.run(4000)
            h = s.host()
            assert not (set(h["kbd"][2:]) - {0}) and h["kbd"][0] == 0 and h["mb"] == 0 and h["queued"] == 0, (f, link, loss, h)
            assert s.out["buttons"] == 0, (f, link, hex(s.out["buttons"]))

if __name__ == "__main__":
    if not FILES: print("no profiles found in", DIR); sys.exit(0)
    print("profiles:", FILES)
    ok = 0
    for t in TESTS:
        try: t(); ok += 1; print("ok  ", t.__name__)
        except Exception: print("FAIL", t.__name__); traceback.print_exc()
    print(f"{ok}/{len(TESTS)} passed"); sys.exit(ok != len(TESTS))
