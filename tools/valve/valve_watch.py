#!/usr/bin/env python3
"""
Track Valve's Steam Controller (Ibex) / Puck (Proteus) firmware releases and keep sc26re in step.

Firmware source: https://github.com/OpenSteamController/Ibex-Firmware (index.json + blobs).

  valve_watch.py check   --state valve/state.json --out OUTDIR      find & download new releases
  valve_watch.py report  OLD.fw NEW.fw [--registry FILE] [--objdump BIN]   markdown change report
  valve_watch.py apply-settings OLD.fw NEW.fw --registry FILE [--header FILE]
  valve_watch.py mark    --state valve/state.json VERSION...           record versions as processed
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fwtools as F  # noqa: E402

REPO_RAW = os.environ.get("VALVE_FW_RAW", "https://raw.githubusercontent.com/OpenSteamController/Ibex-Firmware/main")
ENTRY_RE = re.compile(
    r'^(\s*\[(\d+)\]\s*=\s*(\w*SETTING_ENTRY)\()\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*"([^"]*)"',
    re.M)


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "sc26re-valve-watch"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


# ---------------------------------------------------------------- check / mark
def load_state(path):
    with open(path) as f:
        return json.load(f)


def save_state(path, st):
    with open(path, "w") as f:
        json.dump(st, f, indent=2, sort_keys=True)
        f.write("\n")


def cmd_check(a):
    st = load_state(a.state)
    index = json.loads(fetch(REPO_RAW + "/index.json"))
    os.makedirs(a.out, exist_ok=True)
    result = {"controller": [], "puck": []}
    for fam, key, folder in (("controller", "controller", "Controller"), ("puck", "puck", "Puck")):
        cat = index.get(key) or {}
        seen = set(st.get(fam + "_seen", []))
        base_unix = st.get(fam + "_baseline_unix", 0)
        new = []
        for name, meta in cat.items():
            ver = meta.get("version_hex") or name.split("_")[-1].split(".")[0]
            if ver in seen or meta.get("version_unix", 0) <= base_unix:
                continue
            new.append((meta.get("version_unix", 0), ver, name, meta))
        for _, ver, name, meta in sorted(new):
            data = fetch(f"{REPO_RAW}/{folder}/{name}")
            path = os.path.join(a.out, name)
            with open(path, "wb") as f:
                f.write(data)
            chans = ", ".join(f"{c} {v.get('date', '')[:10]}" for c, v in (meta.get("first_seen") or {}).items())
            result[fam].append({"version": ver, "file": path, "channels": chans})
        if fam == "controller" and result[fam]:
            base = st["controller_baseline"]
            name = f"IBEX_FW_{base}.fw"
            path = os.path.join(a.out, name)
            with open(path, "wb") as f:
                f.write(fetch(f"{REPO_RAW}/Controller/{name}"))
            result["controller_baseline_file"] = path
    with open(os.path.join(a.out, "new.json"), "w") as f:
        json.dump(result, f, indent=2)
    print(json.dumps(result, indent=2))


def cmd_mark(a):
    st = load_state(a.state)
    index = json.loads(fetch(REPO_RAW + "/index.json"))
    for ver in a.versions:
        for fam, key in (("controller", "controller"), ("puck", "puck")):
            for name, meta in (index.get(key) or {}).items():
                if meta.get("version_hex") == ver or ver in name:
                    s = set(st.get(fam + "_seen", []))
                    s.add(ver)
                    st[fam + "_seen"] = sorted(s)
    save_state(a.state, st)


# ---------------------------------------------------------------- report
def registry_entries(path):
    if not path or not os.path.exists(path):
        return {}
    txt = open(path).read()
    return {int(m.group(2)): (int(m.group(4)), int(m.group(5)), int(m.group(6)), m.group(7))
            for m in ENTRY_RE.finditer(txt)}


def disassemble(objdump, fw_path, tmp):
    b = F.payload(open(fw_path, "rb").read())
    binp = os.path.join(tmp, os.path.basename(fw_path) + ".bin")
    open(binp, "wb").write(b)
    dis = subprocess.run([objdump, "-D", "-b", "binary", "-marm", "-Mforce-thumb",
                          f"--adjust-vma=0x{F.BASE:x}", binp],
                         capture_output=True, text=True, check=True).stdout
    return b, dis


def func_diff(objdump, old_fw, new_fw):
    import funcdiff
    with tempfile.TemporaryDirectory() as tmp:
        ob, od = disassemble(objdump, old_fw, tmp)
        nb, nd = disassemble(objdump, new_fw, tmp)
    return funcdiff.diff(ob, od, nb, nd)


def build_report(old_fw, new_fw, registry=None, objdump=None, title=None):
    ob = F.payload(open(old_fw, "rb").read())
    nb = F.payload(open(new_fw, "rb").read())
    reg = registry_entries(registry)
    on = os.path.basename(old_fw)
    nn = os.path.basename(new_fw)
    out = [f"## {title or 'Valve controller firmware ' + nn}", "",
           f"Compared against `{on}` (the last Valve version ported into sc26re).", ""]

    # settings
    oa, oe = F.settings_table(ob)
    na, ne = F.settings_table(nb)
    out.append("### Settings registry")
    if not ne:
        out.append("- settings table **not found** in the new firmware — check manually")
    else:
        rows = []
        for i in range(max(len(oe), len(ne))):
            o = oe[i] if i < len(oe) else None
            n = ne[i] if i < len(ne) else None
            if o == n:
                continue
            r = reg.get(i)
            rows.append(f"| {i} | {r[3] if r else '—'} | {o if o else 'new'} | {n if n else 'removed'} | "
                        f"{r[:3] if r else 'missing'} |")
        if rows:
            out += ["| # | sc26re name | old (def,min,max) | new (def,min,max) | sc26re now |",
                    "|---|---|---|---|---|"] + rows
        else:
            out.append("- no changes")
        out.append(f"- entries: {len(oe)} → {len(ne)}")
    out.append("")

    # feature commands
    of, nf = set(F.feature_table(ob)), set(F.feature_table(nb))
    out.append("### Feature-report commands (Steam → controller)")
    if not nf:
        out.append("- dispatch table **not found** — check manually")
    else:
        add, rem = sorted(nf - of), sorted(of - nf)
        out.append(f"- added: {', '.join(hex(x) for x in add) or 'none'}")
        out.append(f"- removed: {', '.join(hex(x) for x in rem) or 'none'}")
    out.append("")

    # strings
    os_, ns = F.strings(ob), F.strings(nb)
    add = sorted(ns - os_)
    rem = sorted(os_ - ns)
    out.append(f"### Log / name strings: +{len(add)} −{len(rem)}")
    for lbl, lst in (("Added", add), ("Removed", rem)):
        if lst:
            out.append(f"<details><summary>{lbl} ({len(lst)})</summary>\n\n```")
            out += [s[:160] for s in lst[:300]]
            if len(lst) > 300:
                out.append(f"... {len(lst) - 300} more")
            out.append("```\n</details>")
    out.append("")

    # functions
    out.append("### Code changes")
    if objdump and shutil.which(objdump):
        try:
            changed, new_only, stats = func_diff(objdump, old_fw, new_fw)
            out.append(f"- functions: {stats['old']} → {stats['new']}, identical {stats['same']}, "
                       f"changed {len(changed)}, new {len(new_only)}")
            if changed:
                out.append("<details><summary>Changed functions that reference strings "
                           "(most informative first)</summary>\n\n```")
                for o, n, ratio, ss in changed[:120]:
                    out.append(f"0x{o:05x} -> 0x{n:05x}  sim {ratio:.2f}  {ss[:140]}")
                out.append("```\n</details>")
            if new_only:
                out.append("<details><summary>New functions</summary>\n\n```")
                for n, size, ss in new_only[:120]:
                    out.append(f"0x{n:05x}  {size:4d} ins  {ss[:140]}")
                out.append("```\n</details>")
        except Exception as e:  # report must not fail because of the disassembler
            out.append(f"- function diff failed: {e}")
    else:
        out.append("- disassembler not available, function diff skipped")
    out.append("")
    return "\n".join(out)


def cmd_report(a):
    print(build_report(a.old, a.new, a.registry, a.objdump, a.title))


# ---------------------------------------------------------------- apply-settings
def cmd_apply_settings(a):
    ob = F.payload(open(a.old, "rb").read())
    nb = F.payload(open(a.new, "rb").read())
    _, oe = F.settings_table(ob)
    _, ne = F.settings_table(nb)
    if not oe or not ne:
        print("settings table not found, nothing applied")
        return
    txt = open(a.registry).read()
    reg = registry_entries(a.registry)
    changed = []
    for i in range(min(len(oe), len(ne))):
        if oe[i] == ne[i] or i not in reg:
            continue
        cur = reg[i][:3]
        if cur == tuple(ne[i]):
            continue
        if cur != tuple(oe[i]):
            changed.append(f"[{i}] skipped: sc26re has {cur}, Valve {oe[i]} -> {ne[i]} (sc26re differs on purpose?)")
            continue

        def rep(m, i=i):
            if int(m.group(2)) != i:
                return m.group(0)
            return f'{m.group(1)}{ne[i][0]}, {ne[i][1]}, {ne[i][2]}, "{m.group(7)}"'
        txt = ENTRY_RE.sub(rep, txt)
        changed.append(f"[{i}] {reg[i][3]}: {oe[i]} -> {ne[i]}")
    added = []
    if len(ne) > len(oe) and a.header and os.path.exists(a.header):
        last = max(reg) if reg else -1
        new_idx = [i for i in range(len(oe), len(ne)) if i > last]
        if new_idx and new_idx[0] == last + 1:
            m = list(ENTRY_RE.finditer(txt))
            tail = m[-1]
            line_end = txt.index("\n", tail.end()) + 1
            ins = "".join(f'\t[{i}] = UNKNOWN_SETTING_ENTRY({ne[i][0]}, {ne[i][1]}, {ne[i][2]}, '
                          f'"unknown_{i}", NULL), /* new in Valve firmware, auto-added */\n' for i in new_idx)
            txt = txt[:line_end] + ins + txt[line_end:]
            h = open(a.header).read()
            hm = re.search(r"\(IBEX_SETTING_SDL_COUNT \+ (\d+)\)", h)
            if hm:
                h = h[:hm.start(1)] + str(int(hm.group(1)) + len(new_idx)) + h[hm.end(1):]
                open(a.header, "w").write(h)
                added = [f"[{i}] new entry {ne[i]}" for i in new_idx]
            else:
                txt = open(a.registry).read()  # cannot bump the count, keep file untouched
    open(a.registry, "w").write(txt)
    for line in changed + added:
        print(line)
    if not changed and not added:
        print("no settings changes")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--state", required=True)
    c.add_argument("--out", required=True)
    m = sub.add_parser("mark")
    m.add_argument("--state", required=True)
    m.add_argument("versions", nargs="+")
    r = sub.add_parser("report")
    r.add_argument("old")
    r.add_argument("new")
    r.add_argument("--registry")
    r.add_argument("--objdump", default="arm-none-eabi-objdump")
    r.add_argument("--title")
    s = sub.add_parser("apply-settings")
    s.add_argument("old")
    s.add_argument("new")
    s.add_argument("--registry", required=True)
    s.add_argument("--header")
    a = p.parse_args()
    {"check": cmd_check, "mark": cmd_mark, "report": cmd_report,
     "apply-settings": cmd_apply_settings}[a.cmd](a)


if __name__ == "__main__":
    main()
