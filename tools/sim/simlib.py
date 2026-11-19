"""Virtual Steam Controller: drives the real remap.c (libsim.so) at 1 kHz."""
import ctypes, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
RFIELDS = ["buttons", "tl", "tr", "lsx", "lsy", "rsx", "rsy", "lpx", "lpy", "lpp", "rpx", "rpy", "rpp",
           "ax", "ay", "az", "gx", "gy", "gz", "qw", "qx", "qy", "qz", "ts"]
RFMT = "<Ihhhhhhhh" + "H" + "hh" + "H" + "hhhhhhhhhh" + "I"
RSIZE = struct.calcsize(RFMT)

BTN = dict(A=0, B=1, X=2, Y=3, QAM=4, R3=5, MENU=6, R4=7, R5=8, RB=9, DDOWN=10, DRIGHT=11, DLEFT=12, DUP=13,
           VIEW=14, L3=15, STEAM=16, L4=17, L5=18, LB=19, RS_TOUCH=20, RP_TOUCH=21, RP_CLICK=22, RT_CLICK=23,
           LS_TOUCH=24, LP_TOUCH=25, LP_CLICK=26, LT_CLICK=27, RGRIP=28, LGRIP=29)
SRC = dict(BTN, LS_UP=30, LS_DOWN=31, LS_LEFT=32, LS_RIGHT=33, RS_UP=34, RS_DOWN=35, RS_LEFT=36, RS_RIGHT=37,
           LP_UP=38, LP_DOWN=39, LP_LEFT=40, LP_RIGHT=41, RP_UP=42, RP_DOWN=43, RP_LEFT=44, RP_RIGHT=45,
           LP_G0=46, LP_G1=47, LP_G2=48, LP_G3=49, RP_G0=50, RP_G1=51, RP_G2=52, RP_G3=53,
           LP_CW=54, LP_CCW=55, RP_CW=56, RP_CCW=57, LT_SOFT=58, RT_SOFT=59,
           LS_RING=60, RS_RING=61, LP_RING=62, RP_RING=63, LP_CENTER=64, RP_CENTER=65,
           GYRO_UP=66, GYRO_DOWN=67, GYRO_LEFT=68, GYRO_RIGHT=69,
           LS_UR=70, LS_DR=71, LS_DL=72, LS_UL=73, RS_UR=74, RS_DR=75, RS_DL=76, RS_UL=77)
def v8_of(d):
    """a v6 (2056 bytes) or v7 (3144) profile as the firmware and the page upgrade it: v7 area empty for v6,
    the v8 shifted clicks (L3, R3, LP_CLICK, RP_CLICK) copies of the normal ones with their timing"""
    b = bytearray(3232); b[:len(d)] = d; b[1] = 8
    for k, src in enumerate((15, 5, 26, 22)):
        b[3144 + 14 * k:3158 + 14 * k] = b[488 + 14 * src:502 + 14 * src]
        b[3200 + 6 * k:3206 + 6 * k] = b[2056 + 6 * src:2062 + 6 * src]
    return bytes(b)
# keyboard usages
def key(c):
    if len(c) == 1 and c.isalpha(): return 0x04 + ord(c.lower()) - ord('a')
    return {"1": 0x1E, "2": 0x1F, "3": 0x20, "4": 0x21, "5": 0x22, "space": 0x2C, "enter": 0x28, "esc": 0x29,
            "tab": 0x2B, "lshift": 0xE1, "lctrl": 0xE0}[c]
MOUSE_L, MOUSE_R, WHEEL_UP, WHEEL_DOWN = 0x70, 0x71, 0x72, 0x73
MOVE_UP, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT, MOUSE_M = 0x76, 0x77, 0x78, 0x79, 0x7A
PAD, XBOX, PROFILE, LAYER, MACRO, STICK, TRIG_L, TRIG_R = 0x80, 0xA0, 0xC0, 0xC4, 0xC8, 0xD0, 0xD8, 0xD9
XTRIG_L, XTRIG_R = 0xDA, 0xDB
M_HOLD, M_DOUBLE, M_TOGGLE, M_START, M_RELEASE, M_TURBO, M_CYCLE = 1, 2, 4, 8, 16, 0x20, 0x40
PASS, DPAD4, DPAD8, MOUSE, SCROLL, GATES, JOYSTICK, FLICK, SWIPE, GRID, RADIAL = range(11)
SINGLE, XSTICK, REGION = 11, 12, 13
XTRIG_L, XTRIG_R = 0xDA, 0xDB
AF_CLICK, AF_RING_INV, AF_OTHER, AF_SWREP, AF_PRESS, AF_INVX, AF_INVY, AF_TRACKBALL = 1, 2, 4, 8, 16, 32, 64, 128
PF_BLOCK, PF_GYRO, PF_GYRO_ON = 1, 2, 4


class Profile:
    SIZE = 3232
    SCLICK, SCLICK_BT = 3144, 3200      # v8: shifted clicks of LS, RS, LP, RP (sources 15, 5, 26, 22) x14, timing x6
    EXT = 2056                      # v7 area: bt[124] x6, xbind[12] x14, anx[12] x12, gyx 16, trig_curve, screen
    BT, XBIND, ANX, GYX, TCURVE, SCREEN = 2056, 2800, 2968, 3112, 3128, 3132

    def __init__(self, name="test"):
        self.b = bytearray(self.SIZE)
        self.b[0], self.b[1] = 0xA7, 8
        self.b[2:2 + len(name)] = name.encode()

    def u8(self, o, v): self.b[o] = v & 255
    def u16(self, o, v): struct.pack_into("<H", self.b, o, v & 0xFFFF)
    def s16(self, o, v): struct.pack_into("<h", self.b, o, v)

    def flags(self, f): self.u8(17, f)
    def dpad4(self, on=1): self.u8(16, on)
    def trig_soft(self, side, pct): self.u16(18 + 2 * side, int(pct * 100))
    def move_speed(self, v): self.u16(38, v)
    def trig_mode(self, side, m, hip_ms=0): self.u8(104 + side, m); self.u16(106 + 2 * side, hip_ms)
    def times(self, hold=0, dbl=0, turbo=0): self.u16(118, hold); self.u16(120, dbl); self.u16(122, turbo)
    def macro(self, m, steps):
        for i, (a, t10) in enumerate(steps):
            self.b[128 + m * 32 + i * 2] = a; self.b[128 + m * 32 + i * 2 + 1] = t10

    def analog(self, i, mode, flags=0, dz_in=0, dz_out=0, speed=0, haptic=0, rotate=0, curve=0, ring=0,
               anti=0, press=0, base=None):
        o = 256 + 18 * i if base is None else base
        struct.pack_into("<BBHHHBbHHHH", self.b, o, mode, flags, dz_in, dz_out, speed, haptic, rotate, curve,
                         ring, anti, press)

    def btime(self, src, hold=0, dbl=0, turbo=0, dstart=0, dend=0, interrupt=False, shifted_index=None):
        # per binding timing in ms (0 = the profile's)
        if isinstance(src, str): src = SRC[src]
        k = (src if src < 66 else 112 + src - 66) if shifted_index is None else 66 + shifted_index
        struct.pack_into("<6B", self.b, self.BT + 6 * k, hold // 10, dbl // 10, turbo // 5, dstart // 10, dend // 10,
                         1 if interrupt else 0)

    def anx(self, i, scale_x=0, scale_y=0, edge_spin=0, slices=0, flags=0, region=(0, 0, 0, 0)):
        # i: 0..3 sticks / pads, 4 + g shift group
        struct.pack_into("<HHHBB4B", self.b, self.ANX + 12 * i, scale_x, scale_y, edge_spin, slices, flags, *region)

    def gyx(self, mode=0, space=0, tilt_dz=0, tilt_8way=0, region=(0, 0, 0, 0), region_deg=0):
        struct.pack_into("<4B4BB", self.b, self.GYX, mode, space, tilt_dz, tilt_8way, *region, region_deg)

    def trig_curve(self, side, x100): self.u16(self.TCURVE + 2 * side, x100)
    def screen(self, w, h): self.u16(self.SCREEN, w); self.u16(self.SCREEN + 2, h)

    def shift(self, g, button, dpad4=0, **an):
        o = 328 + 20 * g
        self.b[o] = 0 if button is None else button + 1
        self.b[o + 1] = dpad4
        if an: self.analog(0, base=o + 2, **an)

    def bind(self, src, a0=(), a1=(), a2=(), mode=0, haptic=0, chord=None, shifted_index=None, shifted_click=False):
        # shifted_click: the v8 binding of a stick / pad click (L3, R3, LP_CLICK, RP_CLICK) while its group is shifted
        if isinstance(src, str): src = SRC[src]
        o = (488 + 14 * src) if shifted_index is None else (1412 + 14 * shifted_index)
        if shifted_index is None and src >= 66: o = self.XBIND + 14 * (src - 66)
        if shifted_click: o = self.SCLICK + 14 * [15, 5, 26, 22].index(src)
        self.b[o] = mode
        for g, acts in enumerate((a0, a1, a2)):
            if isinstance(acts, int): acts = (acts,)
            for k in range(4): self.b[o + 1 + g * 4 + k] = acts[k] if k < len(acts) else 0
        self.b[o + 13] = (haptic & 3) | ((0 if chord is None else BTN[chord] + 1) << 2)


SHIFT_SRC = [0, 1, 2, 3, 13, 10, 12, 11, 30, 31, 32, 33, 60, 34, 35, 36, 37, 61,
             38, 39, 40, 41, 46, 47, 48, 49, 54, 55, 62, 64, 42, 43, 44, 45, 50, 51, 52, 53, 56, 57, 63, 65,
             58, 27, 59, 23]


def tag_of(path):
    t = 0xA5
    for c in path.encode(): t = ((t << 1) ^ (t >> 7) ^ c) & 255
    return t


class Sim:
    def __init__(self, fresh=True):
        # a private copy per virtual controller: fresh firmware statics (= power cycle)
        import shutil, tempfile
        Sim._n = getattr(Sim, "_n", 0) + 1
        so = os.path.join(tempfile.gettempdir(), "libsim_%d_%d.so" % (os.getpid(), Sim._n))
        shutil.copy(os.path.join(HERE, os.environ.get("SIMLIB", "libsim.so")), so)
        self.lib = ctypes.CDLL(so)
        os.unlink(so)
        # long runs create thousands of controllers: unload the oldest copies
        import _ctypes
        Sim._libs = getattr(Sim, "_libs", [])
        Sim._libs.append(self.lib)
        while len(Sim._libs) > 64:
            old = Sim._libs.pop(0)
            try: _ctypes.dlclose(old._handle)
            except OSError: pass
        self.lib.sim_events.restype = ctypes.c_char_p
        self.lib.sim_time.restype = ctypes.c_int64
        self.lib.sim_xbox_buttons.restype = ctypes.c_uint32
        assert self.lib.sim_report_size() == RSIZE, (self.lib.sim_report_size(), RSIZE)
        if fresh: self.lib.sim_reset_store()
        if hasattr(self.lib, "sim_reset_link"): self.lib.sim_reset_link()
        self.lib.sim_boot()
        self.state = {k: 0 for k in RFIELDS}
        self.out = dict(self.state)
        self.log = []

    def reboot(self):
        self.lib.sim_boot()

    # ---- feature reports, byte-identical to what web/remap.html sends
    def feature(self, op, path, data=b""):
        p = path.encode()
        req = bytearray(64)
        req[0], req[1] = op, len(p) + 1 + len(data)
        req[2:2 + len(p)] = p
        req[3 + len(p):3 + len(p) + len(data)] = bytes(data)
        rsp = ctypes.create_string_buffer(64)
        r = self.lib.sim_feature(bytes(req), rsp)
        return r, rsp.raw

    def read(self, path):
        r, rsp = self.feature(0xED, path)
        n = rsp[1]
        if n == 0: return None
        assert rsp[2] == tag_of(path), "tag"
        return rsp[3:2 + n]

    def cfg(self, name, v, save=True):
        assert self.feature(0xEE, "sc26re/" + name, struct.pack("<h", v))[0] == 0, name
        if save: assert self.feature(0xEF, "sc26re/" + name)[0] == 0

    def cfg_get(self, name):
        return struct.unpack("<h", self.read("sc26re/" + name)[:2])[0]

    def upload(self, slot, prof):
        b = prof.b
        for o in range(0, len(b), 40):
            r = self.feature(0xEE, "sc26re/rmp/%d/%x" % (slot, o), b[o:o + 40])[0]
            assert r == 0, ("stage", o, r)
        assert self.feature(0xEF, "sc26re/rmp/%d" % slot)[0] == 0

    def download(self, slot):
        out = bytearray()
        o = 0
        while o < Profile.SIZE:
            d = self.read("sc26re/rmp/%d/%x" % (slot, o))
            assert d, o
            out += d
            o += len(d)
        return bytes(out[:Profile.SIZE])

    def use(self, prof, slot=0):
        self.upload(slot, prof)
        self.cfg("remap_profile", slot)
        self.cfg("remap_on", 1)

    # ---- input
    def set(self, **kw):
        for k, v in kw.items():
            if k in BTN:
                b = 1 << BTN[k]
                self.state["buttons"] = (self.state["buttons"] | b) if v else (self.state["buttons"] & ~b)
            else:
                self.state[k] = v

    def press(self, *names):
        self.set(**{n: 1 for n in names})

    def release(self, *names):
        self.set(**{n: 0 for n in names})

    def run(self, ms=1, **kw):
        """advance ms milliseconds at 1 kHz; returns events in that span"""
        if kw: self.set(**kw)
        # what happened between runs (settings commands, profile saves) stays in the log
        self.log += [e for e in self.lib.sim_events().decode().splitlines() if e]
        self.lib.sim_clear_events()
        inb = struct.pack(RFMT, *[self.state[k] for k in RFIELDS])
        outb = ctypes.create_string_buffer(RSIZE)
        jit = getattr(self, "jitter", None)
        left = ms
        while left > 0:
            d = 1 if not jit else min(left, jit())
            self.lib.sim_set_time(self.lib.sim_time() + d)
            self.lib.sim_step(inb, outb, 1000 * d)
            left -= d
        self.out = dict(zip(RFIELDS, struct.unpack(RFMT, outb.raw)))
        ev = [e for e in self.lib.sim_events().decode().splitlines() if e]
        self.lib.sim_clear_events()
        self.log += ev
        return Events(ev)

    def truth(self):
        """degrees the virtual controller turned about x / y / z since the link reset (oracle)"""
        a = (ctypes.c_double * 3)(); self.lib.sim_gyro_truth(a); return [a[0], a[1], a[2]]

    def truth_q(self):
        """the orientation those turns give (w, x, y, z), turns about the controller's own axes"""
        a = (ctypes.c_double * 4)(); self.lib.sim_gyro_truth_q(a); return [a[0], a[1], a[2], a[3]]

    def report42(self):
        """the 0x42 pad report body as the firmware packs it (triton_state_report.h)"""
        b = ctypes.create_string_buffer(64); n = self.lib.sim_report42(b); return b.raw[:n]

    def transport(self, mode, one=0, loss=0, ci=15, seed=1):
        self.lib.sim_transport(mode, one, loss, ci, seed)

    def link(self, up):
        self.lib.sim_link(1 if up else 0)

    def host(self):
        a = (ctypes.c_int32 * 9)()
        self.lib.sim_host(a)
        kb = bytes(ctypes.string_at(ctypes.addressof(a) + 28, 8))
        return dict(mb=a[0], dx=a[1], dy=a[2], wh=a[3], pan=a[4], lost=a[5], queued=a[6], kbd=kb)

    def out_btn(self, name):
        return bool(self.out["buttons"] & (1 << BTN[name]))


class Events(list):
    def kbd(self):
        """sequence of keyboard states as tuples of pressed codes (mods as E0+bit)"""
        res = []
        for e in self:
            p = e.split()
            if p[1] != "K": continue
            mods = int(p[2], 16)
            codes = tuple(sorted([0xE0 + i for i in range(8) if mods >> i & 1] + [int(x, 16) for x in p[3:] if int(x, 16)]))
            res.append(codes)
        return res

    def mouse(self):
        return [tuple(int(x) for x in e.split()[2:]) for e in self if e.split()[1] == "M"]

    def haptics(self):
        return [e for e in self if e.split()[1] == "H"]

    def beeps(self):
        return [e for e in self if e.split()[1] == "B"]

    def taps(self, code):
        """how many times code went down"""
        n, was = 0, False
        for s in self.kbd():
            now = code in s
            if now and not was: n += 1
            was = now
        return n
