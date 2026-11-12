"""Helpers to analyse Valve Ibex (controller) firmware blobs (.fw = 32-byte header + payload @0x8000)."""
import re
import struct

BASE = 0x8000
IBEX_MAGIC = 0xD2D86467
PROTEUS_MAGIC = 0x2E795631


def payload(fw: bytes) -> bytes:
    return fw[32:]


def kind(fw: bytes):
    if len(fw) < 40:
        return None
    m = struct.unpack_from("<I", fw, 0)[0]
    return "ibex" if m == IBEX_MAGIC else "puck" if m == PROTEUS_MAGIC else None


def _runs(b, stride, ok, min_len):
    best = []
    for phase in range(0, stride, 2):
        o = phase
        run = []
        while o + stride <= len(b):
            if ok(b, o):
                run.append(o)
            else:
                if len(run) >= min_len:
                    best.append(list(run))
                run = []
            o += stride
        if len(run) >= min_len:
            best.append(run)
    return best


def settings_table(b: bytes):
    """Settings registry: entries {int16 default, int16 min, int16 max, int16 pad, u32 flash ptr}."""
    flash_end = BASE + len(b)

    def ok(buf, o):
        d, mn, mx, pad, p = struct.unpack_from("<hhhhI", buf, o)
        return pad == 0 and mn <= mx and BASE <= p < flash_end

    runs = _runs(b, 12, ok, 40)
    if not runs:
        return None, []
    run = max(runs, key=len)
    ents = [struct.unpack_from("<hhh", b, o) for o in run]
    return run[0] + BASE, ents


def feature_table(b: bytes):
    """Feature-report dispatch: entries {u32 id < 0x100, u32 setter|0, u32 getter|0} (thumb ptrs)."""
    flash_end = BASE + len(b)

    def isfn(p):
        return p == 0 or (BASE <= p < flash_end and p & 1)

    def ok(buf, o):
        i, s, g = struct.unpack_from("<III", buf, o)
        return 0x80 <= i < 0x100 and isfn(s) and isfn(g) and (s or g)

    runs = _runs(b, 12, ok, 10)
    if not runs:
        return []
    run = max(runs, key=len)
    return sorted({struct.unpack_from("<I", b, o)[0] for o in run})


def strings(b: bytes, min_len=6):
    out = set()
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, b):
        s = m.group().decode()
        # skip noise: needs letters and a word-ish shape
        if sum(c.isalpha() for c in s) >= min_len - 1 and re.search(r"[a-z]{3}", s):
            out.add(s)
    return out
