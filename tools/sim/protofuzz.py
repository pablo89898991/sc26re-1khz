"""Feature-report fuzzing: junk and valid remapper/settings commands mixed with play.
The firmware must not crash (ASan) and the profiles must follow a simple model."""
import random, sys
from simlib import *

PATHS = ["sc26re/rmp/%d/%x", "sc26re/rmp/%d", "sc26re/rmp_out", "sc26re/raw", "sc26re/osd", "sc26re/remap_on", "sc26re/remap_profile",
         "sc26re/gm_sens", "sc26re/rmp/", "sc26re/rmp/9/0", "sc26re/rmp/0/ffff", "sc26re/rmp/0x", "sc26re/", "", "sc26re/rmp/1/7f8", "sc26re/rmp/3/-1"]

def run(seed):
    r = random.Random(seed)
    s = Sim()
    live = [bytes(Profile.SIZE) for _ in range(4)]   # model: committed profiles
    stage = None                                         # (p, bytearray)
    probs = []
    for i in range(1500):
        op = r.random()
        if op < .45:  # valid staging of a chunk
            p = r.randrange(4); o = r.randrange(0, Profile.SIZE, 8) if r.random() < .3 else r.randrange(0, Profile.SIZE // 40) * 40
            n = r.randrange(1, 41)
            data = bytes(r.randrange(256) for _ in range(n))
            if r.random() < .3: data = bytes([0xA7, 6]) + data[2:]
            n = len(data)
            rc = s.feature(0xEE, "sc26re/rmp/%d/%x" % (p, o), data)[0]
            if rc != 0: probs.append("stage rc %d" % rc)
            if stage is None or stage[0] != p: stage = (p, bytearray(live[p]))
            n2 = min(n, Profile.SIZE - o); stage[1][o:o + n2] = data[:n2]
        elif op < .55:  # commit
            p = r.randrange(4)
            s.feature(0xEF, "sc26re/rmp/%d" % p)
            if stage and stage[0] == p:
                if stage[1][0] == 0xA7 and stage[1][1] in (6, 7):   # older profiles: the firmware upgrades them
                    stage = (p, bytearray(v8_of(bytes(stage[1][:Profile.EXT if stage[1][1] == 6 else 3144]))))
                live[p] = bytes(stage[1]); stage = None
        elif op < .7:  # junk
            path = r.choice(PATHS)
            try: path = path % ((r.randrange(-2, 12), r.randrange(0, 70000))[:path.count("%")])
            except TypeError: pass
            data = bytes(r.randrange(256) for _ in range(r.randrange(0, 60 - len(path))))
            opc = r.choice([0xED, 0xEE, 0xEF, 0xF0, 0x12])
            if path.startswith("sc26re/rmp/") and opc in (0xEE, 0xEF):
                # a write the model must know: only the invalid ones here
                pp = path[len("sc26re/rmp/"):]
                valid = len(pp) >= 1 and pp[0] in "0123" and (len(pp) == 1 or (pp[1] == "/" and pp[2:] and all(c in "0123456789abcdef" for c in pp[2:]) and int(pp[2:], 16) < Profile.SIZE))
                if valid: opc = 0xED
            s.feature(opc, path, data)
        elif op < .8:  # reads must show the committed profile
            p = r.randrange(4); o = r.randrange(0, Profile.SIZE // 40) * 40
            d = s.read("sc26re/rmp/%d/%x" % (p, o))
            if d != live[p][o:o + len(d)]: probs.append("read %d/%x differs from the committed profile" % (p, o)); break
        else:  # play
            s.cfg("remap_on", r.randrange(2)); s.cfg("remap_profile", r.randrange(4))
            for _ in range(5):
                s.state["buttons"] = r.randrange(1 << 30) & ~((1 << 16) | (1 << 6))
                s.state["lsx"] = r.randrange(-32767, 32768); s.state["rpx"] = r.randrange(-32767, 32768)
                s.run(r.choice([1, 5, 30]))
    # power cycle: NVS = committed ones (an invalid magic deletes the profile)
    s.reboot(); s.run(5)
    for p in range(4):
        want = live[p] if live[p][0] == 0xA7 else None
        got = s.download(p)
        if want is not None and got != want: probs.append("after reboot profile %d differs" % p)
    return probs

if __name__ == "__main__":
    a, b = int(sys.argv[1]), int(sys.argv[2]); tot = 0
    for seed in range(a, b + 1):
        pr = run(seed); tot += len(pr)
        for x in pr[:3]: print("seed", seed, x)
    print("protofuzz %d-%d: %d problems" % (a, b, tot))
