"""Mutation testing without touching the tree: each mutant is a copy of one firmware file with one
small bug, built into its own library (libsim_mut*.so), and the suites given for it must fail.
usage: mutate2.py [old] [index ...]   (old: the remap.c mutants of mutate.py)"""
import os, re, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "../../app/src")
A = subprocess.run(["gcc", "-print-file-name=libasan.so"], capture_output=True, text=True).stdout.strip()

AUDIT = ["python3", "test_audit.py"]
GYRO = ["python3", "test_gyro.py"]
IMURAW = ["sh", "-c", "gcc -O2 -I$MUTSRC -I../../app/src -o /tmp/mut_imu_raw test_imu_raw.c -lm && /tmp/mut_imu_raw"]
V7 = ["python3", "test_v7.py"]
R1 = ["python3", "test_remap.py"]
R2 = ["python3", "test_remap2.py"]
FUZZ = ["python3", "fuzz.py", "11", "10"]
# (file, original text, mutated text, suites, flavour): flavour "" plain, "gm" with gyro_mouse.c, "asan"
MUTANTS = [
    ("remap.c", "\t\t\tif(radial_sel[2 + i] >= n)\n\t\t\t{\n\t\t\t\tradial_sel[2 + i] = -1;\n\t\t\t}\n", "", [AUDIT], "asan"),
    ("remap.c", "\t\t\tif(radial_sel[j] >= n)\n\t\t\t{\n\t\t\t\tradial_sel[j] = -1; /* chosen in a menu with more slices (a shift or a layer) */\n\t\t\t}\n", "", [AUDIT], ""),
    ("remap.c", "MIN(p->ext.screen[1], 16384U)", "p->ext.screen[1]", [AUDIT], ""),
    ("remap.c", "*value = sc26re_cfg_clamp((enum sc26re_cfg_id)id, v);", "*value = v;", [GYRO], "gm"),
    ("sc26re_cfg.c", "v != SC26RE_CFG_AUTO ? CLAMP(v, descs[i].min, descs[i].max) : descs[i].def;", "v == SC26RE_CFG_AUTO ? v : CLAMP(v, descs[i].min, descs[i].max);", [AUDIT], ""),
    ("sc26re_cfg.c", "\treturn CLAMP(value, descs[id].min, descs[id].max);\n}", "\treturn value;\n}", [GYRO], "gm"),
    ("sc26re_cfg.c", "v = v >= 750 ? 1000 : (v >= 375 ? 500 : 250);", "v = v >= 750 ? 1000 : (v >= 300 ? 500 : 250);", [AUDIT], ""),
    ("sc26re_cfg.c", "\t\t\t\tv = INT16_MAX;", "\t\t\t\tv = 0;", [AUDIT], ""),
    ("gyro_mouse.c", "bool chord = (physical & CHORD_MASK) == CHORD_MASK;", "bool chord = (raw & CHORD_MASK) == CHORD_MASK;", [GYRO], "gm"),
    ("gyro_mouse.c", "uint32_t pressed = physical & ~prev_phys;", "uint32_t pressed = raw & ~prev_phys;", [GYRO], "gm"),
    ("gyro_mouse.c", "if(cal_hide != 0U && (physical & cal_hide) == 0U && cal == CAL_IDLE)", "if(cal_hide != 0U && cal == CAL_IDLE)", [GYRO], "gm"),
    ("gyro_mouse.c", "\t\t\treturn !held;", "\t\t\treturn held;", [GYRO], "gm"),
    ("gyro_mouse.c", "\t\treturn v > h ? v - h : v - l;", "\t\treturn v;", [GYRO], "gm"),
    ("gyro_mouse.c", "\tif(save && cal == CAL_TURNING && cal_counts >= 10.0f)", "\tif(save && cal_counts >= 10.0f && false)", [GYRO], "gm"),
    ("imu_raw.h", "c = 1.0f - h2 * (0.5f - h2 * (1.0f / 24.0f));", "c = 1.0f - h2 * 0.5f;", [IMURAW], ""),
    ("imu_raw.h", "o->r[i] += (float)gyro[i] * k;", "o->r[i] += (float)gyro[i] * k * 1.001f;", [IMURAW], ""),
    ("imu_raw.h", "if(o->r[0] * o->r[0] + o->r[1] * o->r[1] + o->r[2] * o->r[2] >= 1e-6f)", "if(o->r[0] * o->r[0] + o->r[1] * o->r[1] + o->r[2] * o->r[2] >= 1e-2f)", [IMURAW], ""),
    ("imu_raw.h", "\timu_raw_orient_turn(o->q, o->r, q);\n\tfor", "\tq[0] = o->q[0]; q[1] = o->q[1]; q[2] = o->q[2]; q[3] = o->q[3];\n\tfor", [IMURAW], ""),
]

if sys.argv[1:2] == ["old"]:
    # the remap.c mutants of mutate.py with its suites, built here into libsim_mut.so (the tree stays as it is)
    import ast
    vals = {}
    for node in ast.parse(open(os.path.join(HERE, "mutate.py")).read()).body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) and node.targets[0].id in ("MUTANTS", "SUITES"):
            vals[node.targets[0].id] = ast.literal_eval(node.value)
    MUTANTS = [("remap.c", a, b, vals["SUITES"], "") for a, b in vals["MUTANTS"]]
    del sys.argv[1]

def build(tmp, flavour):
    out = os.path.join(HERE, "libsim_mut%s.so" % flavour)
    srcs = ["sim.c", os.path.join(tmp, "remap.c"), os.path.join(SRC, "hwtest.c"), os.path.join(tmp, "sc26re_cfg.c")]
    flags = ["-O1", "-g", "-fPIC", "-shared", "-Wall", "-Wno-unused-function", "-Iinc", "-I" + tmp, "-I" + SRC, "-DSC26RE_SIM", "-include", "zephyr/kernel.h"]
    if flavour == "gm": flags.append("-DWITH_GM"); srcs.append(os.path.join(tmp, "gyro_mouse.c"))
    else: srcs.append(os.path.join(SRC, "xinput_guide.c"))
    if flavour == "asan": flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]
    r = subprocess.run(["gcc"] + flags + ["-o", out] + srcs + ["-lm"], cwd=HERE, capture_output=True, text=True)
    return os.path.basename(out) if r.returncode == 0 else None

survived = []
only = [int(x) for x in sys.argv[1:]]
for i, (f, a, b, suites, flavour) in enumerate(MUTANTS):
    if only and i not in only: continue
    orig = open(os.path.join(SRC, f)).read()
    if orig.count(a) != 1: print("mutant %2d: MISSING in %s: %s" % (i, f, a.strip()[:60])); survived.append(i); continue
    tmp = tempfile.mkdtemp(prefix="mut")
    for g in ("remap.c", "sc26re_cfg.c", "gyro_mouse.c", "imu_raw.h"):
        shutil.copy(os.path.join(SRC, g), tmp)
    open(os.path.join(tmp, f), "w").write(orig.replace(a, b, 1))
    lib = build(tmp, flavour)
    if not lib: print("mutant %2d: does not build" % i); continue
    env = dict(os.environ, SIMLIB=lib, MUTSRC=tmp)
    if flavour == "asan": env.update(LD_PRELOAD=A, ASAN_OPTIONS="detect_leaks=0")
    caught = None
    for sx in suites:
        p = subprocess.run(sx, cwd=HERE, env=env, capture_output=True, text=True, timeout=900)
        out = (p.stdout + p.stderr).strip().splitlines()
        last = out[-1] if out else "crash"
        if p.returncode != 0 or not re.search(r"(^| )0 (failed|problems)|worst", last) or (sx is IMURAW and p.returncode):
            caught = "%s (%s)" % (sx[1] if sx[0] == "python3" else "test_imu_raw", last[:60]); break
    print("mutant %2d %-13s %s  [%s]" % (i, f, "caught by " + caught if caught else "SURVIVED", (b.strip() or "removed: " + a.strip())[:70]), flush=True)
    if not caught: survived.append(i)
    shutil.rmtree(tmp, ignore_errors=True)
print("survived:", survived)
