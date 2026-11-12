"""Fuzzy function-level diff of two Thumb-2 firmware disassemblies (objdump -D -b binary)."""
import collections
import difflib
import hashlib
import re
import struct

BASE = 0x8000
INS_RE = re.compile(r"\s+([0-9a-f]+):\s+([0-9a-f]{4}(?: [0-9a-f]{4})?)\s+(\S+)\s*(.*)")


def _string_at(b, addr):
    o = addr - BASE
    if 0 <= o < len(b):
        e = b.find(b"\0", o, o + 120)
        if e > o + 3:
            s = b[o:e]
            if all(32 <= c < 127 for c in s):
                return s.decode()
    return None


def functions(b, dis):
    ins = []
    for line in dis.splitlines():
        m = INS_RE.match(line)
        if m:
            ins.append((int(m.group(1), 16), m.group(3), m.group(4)))
    pool = set()
    for a, mn, op in ins:
        for m in re.finditer(r"\(0x([0-9a-f]+)\)", op):
            la = int(m.group(1), 16)
            pool.update((la, la + 2))
    ins = [x for x in ins if x[0] not in pool]
    starts = [i for i, (a, mn, op) in enumerate(ins) if mn in ("push", "stmdb") and "lr" in op]
    fs = {}
    for k, si in enumerate(starts):
        ei = starts[k + 1] if k + 1 < len(starts) else len(ins)
        body = ins[si:ei]
        toks, strs = [], []
        for a, mn, op in body:
            if mn.startswith("."):
                continue
            op2 = re.sub(r"0x[0-9a-f]+", "A", op) if (mn.startswith("b") or mn in ("cbz", "cbnz", "adr")) else op
            m = re.search(r"\[pc, #\d+\]\s*@\s*\(?0x([0-9a-f]+)", op)
            if m:
                o = int(m.group(1), 16) - BASE
                if 0 <= o <= len(b) - 4:
                    v = struct.unpack_from("<I", b, o)[0]
                    s = _string_at(b, v)
                    if s:
                        strs.append(s)
                        op2 = 'LIT"%s"' % s
                    elif v < 0x10000:
                        op2 = "LIT#%d" % v
                    else:
                        op2 = "LITPTR"
            toks.append(mn + " " + re.sub(r"@.*", "", op2))
        fs[body[0][0]] = dict(toks=toks, strs=strs, n=len(toks),
                              h=hashlib.md5("\n".join(toks).encode()).hexdigest())
    return fs


def diff(ob, od, nb, nd, max_candidates=4000):
    A, B = functions(ob, od), functions(nb, nd)
    hb = collections.defaultdict(list)
    for a, f in B.items():
        hb[f["h"]].append(a)
    ha = {f["h"] for f in A.values()}
    un_a = [a for a, f in A.items() if f["h"] not in hb]
    un_b = [a for a, f in B.items() if f["h"] not in ha]
    # index new functions by referenced strings for a fast first pass
    by_str = collections.defaultdict(list)
    for x in un_b:
        for s in set(B[x]["strs"]):
            by_str[s].append(x)
    used, pairs = set(), []
    for a in un_a[:max_candidates]:
        fa = A[a]
        cands = {x for s in set(fa["strs"]) for x in by_str.get(s, [])} if fa["strs"] else set(un_b)
        best, bs = None, 0.0
        for x in cands:
            if x in used:
                continue
            fb = B[x]
            if abs(fa["n"] - fb["n"]) > max(20, fa["n"] * 0.5):
                continue
            s = len(set(fa["strs"]) & set(fb["strs"])) * 5
            if s == 0 and fa["strs"]:
                continue
            r = difflib.SequenceMatcher(None, fa["toks"], fb["toks"], autojunk=False).quick_ratio()
            if s + r > bs:
                bs, best = s + r, x
        if best is not None and bs > 0.6:
            used.add(best)
            r = difflib.SequenceMatcher(None, fa["toks"], B[best]["toks"], autojunk=False).ratio()
            pairs.append((a, best, r, " | ".join(sorted(set(fa["strs"])))))
    changed = sorted((p for p in pairs if p[3]), key=lambda p: p[2])
    new_only = sorted(((x, B[x]["n"], " | ".join(sorted(set(B[x]["strs"]))))
                       for x in un_b if x not in used), key=lambda t: (not t[2], -t[1]))
    stats = dict(old=len(A), new=len(B), same=len(A) - len(un_a))
    return changed, new_only, stats
