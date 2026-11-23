"""v51.4 test 23: the profile LED colour and the Puck's charge light (sim_dock models charge_mode.c)."""
from simlib import *
ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1; print("PASS", name)
    else: fail += 1; print("FAIL", name, info)
WHITE, BLUE, AMBER = 0x000000, 0x0008FF, 0x1A1400
def prof():
    p = Profile(); p.bind("A", key("a")); p.u8(40, 0); p.u8(41, 8); p.u8(42, 255); return p
def sim(remap_on):
    s = Sim(); s.lib.sim_led_set(0, 0, 0); s.upload(0, prof()); s.cfg("remap_profile", 0); s.cfg("remap_on", remap_on); s.run(5); return s
led = lambda s: s.lib.sim_led_get()
def toggle(s, on): s.cfg("remap_on", on); s.run(5)
def dock(s, on): s.lib.sim_dock(on); s.run(5)

s = sim(1); check("profile on: blue", led(s) == BLUE, hex(led(s)))
dock(s, 1); check("on the Puck: amber", led(s) == AMBER, hex(led(s)))
dock(s, 0); check("lifted: blue", led(s) == BLUE, hex(led(s)))
toggle(s, 0); check("remapper off: base colour", led(s) == WHITE, hex(led(s)))

# the bug: remapper switched off while on the Puck, then lifted
s = sim(1); dock(s, 1); toggle(s, 0)
check("off on the Puck: charge light stays", led(s) == AMBER, hex(led(s)))
dock(s, 0); check("off on the Puck, lifted: base colour", led(s) == WHITE, hex(led(s)))
toggle(s, 1); check("then on again: blue", led(s) == BLUE, hex(led(s)))
toggle(s, 0); check("then off again: base colour (not blue)", led(s) == WHITE, hex(led(s)))

# remapper switched on while on the Puck
s = sim(0); dock(s, 1); toggle(s, 1)
check("on on the Puck: charge light stays", led(s) == AMBER, hex(led(s)))
dock(s, 0); check("on on the Puck, lifted: blue", led(s) == BLUE, hex(led(s)))
toggle(s, 0); check("then off: base colour (not amber)", led(s) == WHITE, hex(led(s)))
print(); print("%d passed, %d failed" % (ok, fail))
