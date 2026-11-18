"""Mutation testing: put a small bug in remap.c, rebuild, run the quick suites; every mutant must be caught."""
import subprocess, sys, re, os, shutil
SRC = "../../app/src/remap.c"
# works on a copy of the tree file; the original is put back at the end (also on SIGTERM)
MUTANTS = [
    ("now - s->t_down >= HOLD_MS", "now - s->t_down > HOLD_MS + 5"),
    ("now - s->t_up <= DOUBLE_MS", "now - s->t_up < DOUBLE_MS - 5"),
    ("#define TAP_MS 40", "#define TAP_MS 4"),
    ("if(fabsf(fy) >= fabsf(fx))", "if(fabsf(fy) > fabsf(fx) * 2.0f)"),
    ("return m8[((int)((a + 22.5f) / 45.0f)) & 7];", "return m8[((int)((a) / 45.0f)) & 7];"),
    ("return fy >= 0.0f ? (fx < 0.0f ? 1U : 2U) : (fx < 0.0f ? 4U : 8U);", "return fy >= 0.0f ? (fx < 0.0f ? 2U : 1U) : (fx < 0.0f ? 4U : 8U);"),
    ("		return m2 < r * r && m2 >= dz * dz;", "		return m2 < r * r;"),
    ("	return m2 >= r * r;", "	return m2 > r * r * 1.2f;"),
    ("		if(*nk < 6U && memchr(keys, a, *nk) == NULL)", "		if(*nk < 6U)"),
    ("		kbd[0] |= kbd_pend[0];\n		for(int i = 2; i < 8 && kbd_pend[i]; ++i)", "		for(int i = 2; i < 2 && kbd_pend[i]; ++i)"),
    ("		mouse_pend |= mouse_buttons & ~last_buttons;", "		(void)0;"),
    ("				release_all();\n				was_on = false;", "				was_on = false;"),
    ("	if(staging_p != p)\n	{\n		staging = profiles[p];", "	if(staging_p != p)\n	{\n		memset(&staging, 0, sizeof(staging));"),
    ("		else if(s->waiting_double)\n		{", "		else if(false)\n		{"),
    ("	if(now < ss->pulse_until + REPEAT_GAP_MS && ss->pulse_group == 0)", "	if(false)"),
    ("		flick_left = CLAMP(flick_left + c360 * a / 360.0f, -c360, c360);", "		flick_left += c360 * a / 361.0f;"),
    ("	if(l != NULL && bound(&l->bind[s]) && !has_layer_action(b) && !has_layer_action(&l->bind[s]))", "	if(l != NULL && bound(&l->bind[s]))"),
    ("&& analog_of_src(layer_holder) != i)", ")"),
    ("			if(last_active != NULL)\n			{\n				release_all();\n				block_pressed = true;", "			if(last_active != NULL)\n			{\n				release_all();"),
    ("			drop_hold(ss);\n			if(radial_fire", "			if(radial_fire"),
    ("	mouse_acc[0] = CLAMP(mouse_acc[0], -32767.0f, 32767.0f);", ""),
    ("	if((int64_t)dx * dx + (int64_t)dy * dy < (int64_t)th * th)", "	if(dx * dx + dy * dy < th * th)"),
    ("	TURBO_MS = MAX(p->turbo_ms ? p->turbo_ms : 100U, 20U);", "	TURBO_MS = 100U;"),
    ("	mon_pad = 0U;            /* the output monitor shows nothing held either */", ""),
    ("	xbox_buttons = 0U;\n	move_dirs = 0U;\n	stick_out = 0U;", "	move_dirs = 0U;\n	stick_out = 0U;"),
    ("	mm = MIN((m - dz) / (1.0f - dz), 1.0f);", "	mm = MIN(m, 1.0f);"),
    ("		float k = (float)an->speed * 0.01f * dt * mm;", "		float k = (float)an->speed * 0.01f * dt;"),
    ("radial_sel[j] = (int8_t)(((int)((a + 180.0f / (float)n) / (360.0f / (float)n))) % n);", "radial_sel[j] = (int8_t)(((int)((a) / (360.0f / (float)n))) % n);"),
]
SUITES = [["python3", "test_remap.py"], ["python3", "test_remap2.py"], ["python3", "prop.py", "1", "15"], ["python3", "prop2.py", "1", "25"],
          ["python3", "protofuzz.py", "1", "6"], ["python3", "fuzz.py", "7", "8"], ["python3", "wfuzz.py", "7", "1", "x", "6"], ["python3", "wfuzz.py", "7", "2", "d", "4"]]
import signal
signal.signal(signal.SIGTERM, lambda *a: (_ for _ in ()).throw(KeyboardInterrupt()))
orig = open(SRC).read()
shutil.copy(SRC, "/tmp/remap.c.orig")
survived = []
try:
    only = [int(x) for x in sys.argv[1:]]
    for i, (a, b) in enumerate(MUTANTS):
        if only and i not in only: continue
        if a not in orig: print("MISSING", i, a[:60]); continue
        open(SRC, "w").write(orig.replace(a, b, 1))
        if subprocess.run(["make", "-s", "libsim.so"], capture_output=True).returncode:
            print("mutant %2d: does not build" % i); continue
        caught = None
        for sx in SUITES:
            out = subprocess.run(sx, capture_output=True, text=True, timeout=600).stdout + ""
            last = out.strip().splitlines()[-1] if out.strip() else "crash"
            if not re.search(r"(^| )0 (failed|problems)", last):
                caught = sx[1]; break
        print("mutant %2d: %s  (%s)" % (i, "caught by " + caught if caught else "SURVIVED", b.strip()[:60]), flush=True)
        if not caught: survived.append(i)
finally:
    open(SRC, "w").write(orig)
    subprocess.run(["make", "-s", "libsim.so"])
print("survived:", survived)
