/* Steam Input controller configurations (.vdf, "controller_mappings") <-> sc26re remapper profiles.
 * Used by remap.html; also loads in node (module.exports) for the tests in tools/sim/test_vdf.js.
 * Profile layout: struct remap_profile in app/src/remap.h (v8, 3232 bytes).
 *
 * What maps 1:1: keys, modifiers, mouse buttons / wheel, gamepad buttons, mode shifts, action
 * sets (profiles), hold layers, Full / Long / Double / Start / release / Soft press, toggle, turbo,
 * delay start / end, long press and double press times, interruptible, haptics, stick / pad modes
 * (joystick, mouse, scroll wheel, D-pad, button pad, flick stick, radial menu, single button,
 * mouse region, outer ring), trigger soft / full pull, LED, gyro to mouse (sensitivity, space,
 * activation). What cannot: Steam Input API "game actions" (the game names them, Steam knows the
 * keys) and Steam's own commands (screenshots, overlay, on-screen keyboard) - they are listed. */
(function (root) {
  "use strict";
  const PROF_SIZE = 3232, BT = 2056, XBIND = 2800, ANX = 2968, GYX = 3112, TCURVE = 3128;
  /* v8: shifted clicks of the left / right stick and the left / right pad (sources 15, 5, 26, 22) and their timing */
  const SCLICK = 3144, SCLICK_BT = 3200, SCLICK_SRC = [15, 5, 26, 22];
  const AN_OFF = i => 256 + 18 * i, SH_OFF = g => 328 + 20 * g;
  const B_OFF = s => s < 66 ? 488 + 14 * s : XBIND + 14 * (s - 66);
  const SB_OFF = k => 1412 + 14 * k;
  /* shifted sources, same order as remap_shift_src in app/src/remap.c */
  const SHIFT_SRC = [0, 1, 2, 3, 13, 10, 12, 11, 30, 31, 32, 33, 60, 34, 35, 36, 37, 61,
    38, 39, 40, 41, 46, 47, 48, 49, 54, 55, 62, 64, 42, 43, 44, 45, 50, 51, 52, 53, 56, 57, 63, 65, 58, 27, 59, 23];
  const SHIFT_FIRST = [0, 4, 8, 13, 18, 30, 42, 44, 46];

  /* ---------------- VDF text ---------------- */
  /* parsed as nodes: { list: [[key, value], ...] } where value is a string or a node; keys repeat */
  function parse(text) {
    let i = 0;
    const n = text.length;
    function ws() {
      for (;;) {
        while (i < n && /\s/.test(text[i])) i++;
        if (text[i] === "/" && text[i + 1] === "/") { while (i < n && text[i] !== "\n") i++; continue; }
        break;
      }
    }
    function str() {
      ws();
      if (text[i] === '"') {
        i++; let s = "";
        while (i < n && text[i] !== '"') { if (text[i] === "\\" && i + 1 < n) { const c = text[i + 1]; s += c === "n" ? "\n" : c === "t" ? "\t" : c; i += 2; } else s += text[i++]; }
        i++; return s;
      }
      let s = ""; while (i < n && !/[\s{}"]/.test(text[i])) s += text[i++]; return s;
    }
    function node() {
      const list = [];
      for (;;) {
        ws();
        if (i >= n || text[i] === "}") { i++; return { list }; }
        const k = str(); ws();
        if (text[i] === "{") { i++; list.push([k, node()]); }
        else list.push([k, str()]);
      }
    }
    const top = node();
    return top;
  }
  const get = (nd, k) => { if (!nd || !nd.list) return undefined; for (const [kk, v] of nd.list) if (kk.toLowerCase() === k.toLowerCase()) return v; return undefined; };
  const all = (nd, k) => !nd || !nd.list ? [] : nd.list.filter(([kk]) => kk.toLowerCase() === k.toLowerCase()).map(([, v]) => v);
  const entries = nd => nd && nd.list ? nd.list : [];
  function write(nd, depth = 0) {
    const t = "\t".repeat(depth); let o = "";
    for (const [k, v] of nd.list) {
      if (typeof v === "string") o += `${t}"${k}"\t\t"${v.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"\n`;
      else o += `${t}"${k}"\n${t}{\n${write(v, depth + 1)}${t}}\n`;
    }
    return o;
  }
  const N = (...list) => ({ list });

  /* ---------------- names ---------------- */
  const KEYS = {
    ESCAPE: 0x29, ESC: 0x29, RETURN: 0x28, ENTER: 0x28, TAB: 0x2B, SPACE: 0x2C, BACKSPACE: 0x2A, DELETE: 0x4C, INSERT: 0x49, HOME: 0x4A, END: 0x4D,
    PAGE_UP: 0x4B, PAGEUP: 0x4B, PAGE_DOWN: 0x4E, PAGEDOWN: 0x4E, UP_ARROW: 0x52, DOWN_ARROW: 0x51, LEFT_ARROW: 0x50, RIGHT_ARROW: 0x4F,
    UP: 0x52, DOWN: 0x51, LEFT: 0x50, RIGHT: 0x4F, CAPSLOCK: 0x39, CAPS_LOCK: 0x39, PRINTSCREEN: 0x46, PRINT_SCREEN: 0x46, SCROLLLOCK: 0x47, SCROLL_LOCK: 0x47,
    PAUSE: 0x48, DASH: 0x2D, MINUS: 0x2D, EQUALS: 0x2E, LEFT_BRACKET: 0x2F, RIGHT_BRACKET: 0x30, BACKSLASH: 0x31, BACK_SLASH: 0x31,
    SEMICOLON: 0x33, SINGLE_QUOTE: 0x34, APOSTROPHE: 0x34, BACK_TICK: 0x35, BACKTICK: 0x35, GRAVE: 0x35, COMMA: 0x36, PERIOD: 0x37,
    FORWARD_SLASH: 0x38, SLASH: 0x38, NUMLOCK: 0x53, NUM_LOCK: 0x53, KEYPAD_DIVIDE: 0x54, KEYPAD_MULTIPLY: 0x55, KEYPAD_MINUS: 0x56,
    KEYPAD_PLUS: 0x57, KEYPAD_ENTER: 0x58, KEYPAD_PERIOD: 0x63, APPS: 0x65, MENU: 0x65,
  };
  for (let c = 0; c < 26; c++) KEYS[String.fromCharCode(65 + c)] = 4 + c;
  for (let d = 1; d <= 9; d++) { KEYS[String(d)] = 0x1D + d; KEYS["KEY_" + d] = 0x1D + d; KEYS["KEYPAD_" + d] = 0x58 + d; }
  KEYS["0"] = 0x27; KEYS.KEY_0 = 0x27; KEYS.KEYPAD_0 = 0x62;
  for (let f = 1; f <= 12; f++) KEYS["F" + f] = 0x39 + f;
  const MODKEYS = { LEFT_CONTROL: 0xE0, LCONTROL: 0xE0, LEFT_CTRL: 0xE0, LEFT_SHIFT: 0xE1, LSHIFT: 0xE1, LEFT_ALT: 0xE2, LALT: 0xE2,
    LEFT_WINDOWS: 0xE3, LWIN: 0xE3, LEFT_GUI: 0xE3, RIGHT_CONTROL: 0xE4, RCONTROL: 0xE4, RIGHT_CTRL: 0xE4, RIGHT_SHIFT: 0xE5, RSHIFT: 0xE5,
    RIGHT_ALT: 0xE6, RALT: 0xE6, RIGHT_WINDOWS: 0xE7, RWIN: 0xE7, RIGHT_GUI: 0xE7 };
  const KEYNAME = {}; /* usage -> canonical Steam name for export */
  for (const [k, v] of Object.entries(KEYS)) if (!(v in KEYNAME)) KEYNAME[v] = k;
  for (const [k, v] of Object.entries(MODKEYS)) if (!(v in KEYNAME)) KEYNAME[v] = k;
  const MOUSE = { LEFT: 0x70, RIGHT: 0x71, MIDDLE: 0x7A, BACK: 0x7B, X1: 0x7B, BUTTON_4: 0x7B, FORWARD: 0x7C, X2: 0x7C, BUTTON_5: 0x7C };
  const MOUSENAME = { 0x70: "LEFT", 0x71: "RIGHT", 0x7A: "MIDDLE", 0x7B: "BACK", 0x7C: "FORWARD" };
  const WHEEL = { SCROLL_UP: 0x72, SCROLL_DOWN: 0x73, SCROLL_LEFT: 0x74, SCROLL_RIGHT: 0x75 };
  const WHEELNAME = { 0x72: "SCROLL_UP", 0x73: "SCROLL_DOWN", 0x74: "SCROLL_LEFT", 0x75: "SCROLL_RIGHT" };
  /* gamepad buttons (Steam's virtual Xbox pad) -> controller button bits */
  const PADBTN = { A: 0, B: 1, X: 2, Y: 3, SHOULDER_LEFT: 19, SHOULDER_RIGHT: 9, TRIGGER_LEFT: 27, TRIGGER_RIGHT: 23, LEFT_TRIGGER: 27, RIGHT_TRIGGER: 23,
    DPAD_UP: 13, DPAD_DOWN: 10, DPAD_LEFT: 12, DPAD_RIGHT: 11, JOYSTICK_LEFT: 15, JOYSTICK_RIGHT: 5, LEFT_STICK: 15, RIGHT_STICK: 5,
    START: 6, SELECT: 14, BACK: 14, HOME: 16, GUIDE: 16 };
  const PADNAME = { 0: "A", 1: "B", 2: "X", 3: "Y", 19: "SHOULDER_LEFT", 9: "SHOULDER_RIGHT", 27: "TRIGGER_LEFT", 23: "TRIGGER_RIGHT",
    13: "DPAD_UP", 10: "DPAD_DOWN", 12: "DPAD_LEFT", 11: "DPAD_RIGHT", 15: "JOYSTICK_LEFT", 5: "JOYSTICK_RIGHT", 6: "START", 14: "SELECT", 16: "HOME" };
  /* switches inputs -> button bits (the Steam Deck / Steam Controller 2026 names) */
  /* button_escape is the ☰ (Start) button of Steam's controllers, button_menu the ⧉ (View / Select) one */
  const SWITCH = { button_escape: 6, button_menu: 14, left_bumper: 19, right_bumper: 9, button_back_left_upper: 17, button_back_left: 18,
    button_back_right_upper: 7, button_back_right: 8, button_capture: 4 };
  const SWITCHNAME = Object.fromEntries(Object.entries(SWITCH).map(([k, v]) => [v, k]));
  /* Steam's gyro activation mask bit -> button bit, and back */
  /* Steam's k_eGamepadButtonBitMask_* (read from the Steam client's own table): 0 RT, 1 LT, 2 RB, 3 LB,
   * 4 / 5 / 6 / 7 north / east / west / south face, 8..11 D-pad up / right / left / down, 12 View,
   * 13 Steam, 14 Menu, 15 / 16 lower back buttons, 17 / 18 pad clicks, 19 / 20 pad touches, 22 / 26
   * stick clicks, 24 / 25 RT / LT soft pull, 30 / 31 left / right stick deflection, 41 / 42 upper back
   * buttons, 44 / 45 grip touch (this controller), 46 / 47 stick touches. Values: firmware raw button
   * bits; 130 / 131 = soft pull LT / RT (mask bits 30 / 31 of the profile); 140 / 141 = stick pushed */
  const GYRO_MASK_BTN = { 0: 23, 1: 27, 2: 9, 3: 19, 4: 3, 5: 1, 6: 2, 7: 0, 8: 13, 9: 11, 10: 12, 11: 10, 12: 14, 13: 16, 14: 6, 15: 18, 16: 8, 17: 26, 18: 22, 19: 25, 20: 21, 22: 15, 24: 131, 25: 130, 26: 5, 30: 140, 31: 141, 41: 17, 42: 7, 44: 29, 45: 28, 46: 24, 47: 20 };
  const GYRO_MASK_BIT = Object.fromEntries(Object.entries(GYRO_MASK_BTN).map(([k, v]) => [v, +k]));
  const BTN_NAME = { 0: "A", 1: "B", 2: "X", 3: "Y", 6: "Menu", 7: "R4", 8: "R5", 9: "RB", 10: "D-pad down", 11: "D-pad right", 12: "D-pad left", 13: "D-pad up",
    14: "View", 15: "L3", 16: "Steam", 17: "L4", 18: "L5", 19: "LB", 20: "right stick touch", 21: "right pad touch", 22: "right pad click", 23: "RT click",
    24: "left stick touch", 25: "left pad touch", 26: "left pad click", 27: "LT click" };
  const DIAMOND = { button_a: 0, button_b: 1, button_x: 2, button_y: 3 };
  /* Steam sources -> analog index (0 LS, 1 RS, 2 LP, 3 RP) / shift group */
  const SOURCES = { switch: { kind: "switch" }, button_diamond: { kind: "diamond", g: 0 }, dpad: { kind: "dpad", g: 1 },
    joystick: { kind: "an", i: 0, g: 2 }, left_joystick: { kind: "an", i: 0, g: 2 }, right_joystick: { kind: "an", i: 1, g: 3 },
    left_trackpad: { kind: "an", i: 2, g: 4 }, right_trackpad: { kind: "an", i: 3, g: 5 },
    left_trigger: { kind: "trig", t: 0, g: 6 }, right_trigger: { kind: "trig", t: 1, g: 7 }, gyro: { kind: "gyro" } };
  /* Steam's response curves (curve_exponent is a choice, not an exponent) -> our exponent x100 */
  const CURVE = [100, 60, 150, 200, 300];
  const curveIn = (c, custom) => c == null ? 0 : c === 5 ? Math.round((custom || 1) * 100) : (CURVE[c] || 0);
  const curveOut = e => { let k = 0; for (let j = 0; j < CURVE.length; j++) if (Math.abs(CURVE[j] - e) < Math.abs(CURVE[k] - e)) k = j; return k; };
  /* Steam's long press time when a layout does not give one (Steamworks activators doc: 0.15 s) */
  const STEAM_LONG_MS = 150;
  const AN_SRC = ["joystick", "right_joystick", "left_trackpad", "right_trackpad"];
  const AN_BASE = [30, 34, 38, 42];

  /* ---------------- helpers on profile bytes ---------------- */
  function emptyProfile(name) {
    const b = new Uint8Array(PROF_SIZE), dv = new DataView(b.buffer);
    b[0] = 0xA7; b[1] = 8;
    const nm = new TextEncoder().encode(String(name || "").slice(0, 13)); b.set(nm.slice(0, 13), 2);
    dv.setUint16(18, 5000, true); dv.setUint16(20, 5000, true);
    for (let j = 0; j < 2; j++) { dv.setInt16(22 + j * 8, -3333, true); dv.setInt16(24 + j * 8, 3333, true); dv.setInt16(26 + j * 8, 3333, true); dv.setInt16(28 + j * 8, -3333, true); }
    for (let i = 0; i < 4; i++) { const o = AN_OFF(i); dv.setUint16(o + 2, i >= 2 ? 2000 : 0, true); dv.setUint16(o + 6, i >= 2 ? 1200 : 1500, true); dv.setUint16(o + 16, 3000, true); }
    return b;
  }
  const u16 = (b, o) => b[o] | (b[o + 1] << 8);
  const setU16 = (b, o, v) => { v = Math.max(0, Math.min(65535, Math.round(v))); b[o] = v & 255; b[o + 1] = v >> 8; };
  const setS16 = (b, o, v) => { v = Math.max(-32768, Math.min(32767, Math.round(v))); b[o] = v & 255; b[o + 1] = (v >> 8) & 255; };
  const s16 = (b, o) => (u16(b, o) << 16) >> 16;
  const btIndex = (s, shiftedK) => shiftedK != null ? 66 + shiftedK : (s < 66 ? s : 112 + s - 66);
  function shiftIndex(s) { const k = SHIFT_SRC.indexOf(s); return k; }

  /* ---------------- import ---------------- */
  /* binding string -> { act } or { modeShift } or { layer } or { profile } or { led } or { unsupported } */
  function parseBinding(str, ctx) {
    const parts = String(str).split(",")[0].trim().split(/\s+/);
    const kind = (parts[0] || "").toLowerCase(), a = (parts[1] || "").toUpperCase();
    if (kind === "key_press") {
      if (a in MODKEYS) return { act: MODKEYS[a] };
      if (a in KEYS) return { act: KEYS[a] };
      return { unsupported: "key " + a };
    }
    if (kind === "mouse_button") return a in MOUSE ? { act: MOUSE[a] } : { unsupported: "mouse button " + a };
    if (kind === "mouse_wheel") return a in WHEEL ? { act: WHEEL[a] } : { unsupported: "mouse wheel " + a };
    if (kind === "xinput_button" || kind === "gamepad_button") {
      if (!(a in PADBTN)) return { unsupported: "gamepad button " + a };
      ctx.usesPad = true;
      return { act: (ctx.padXbox ? 0xA0 : 0x80) + PADBTN[a] };
    }
    if (kind === "mode_shift") return { modeShift: { source: (parts[1] || "").toLowerCase(), group: parts[2] } };
    if (kind === "controller_action") {
      const c = a.toLowerCase(), n = +parts[2];
      if (c === "change_preset") return { profile: n - 1 };
      if (c === "hold_layer") return { layer: n - 1 };
      if (c === "add_layer") return { layer: n - 1, approx: "“add layer” became “hold layer” (the layer is on while the input is held)" };
      if (c === "remove_layer") return { unsupported: "remove layer (layers end when their input is let go)" };
      if (c === "set_led") return { led: [+parts[2] || 0, +parts[3] || 0, +parts[4] || 0] };
      return { unsupported: "Steam command " + parts[1] };
    }
    if (kind === "game_action") {
      const g = ctx.gameMap && ctx.gameMap[parts[2]];
      if (g) return { act: g };
    }
    if (kind === "game_action") return { unsupported: "game action " + (parts[2] || "") + " (Steam Input API: the game decides the key)", game: true };
    return { unsupported: str };
  }

  class Importer {
    constructor(root, opts) {
      this.root = get(root, "controller_mappings") || root;
      this.opts = Object.assign({ padXbox: true, slots: null }, opts || {});
      this.groups = {};
      for (const g of all(this.root, "group")) { const id = get(g, "id"); if (id != null) this.groups[id] = g; }
      this.presets = all(this.root, "preset").map(p => ({ id: +get(p, "id"), name: get(p, "name"), gsb: get(p, "group_source_bindings") }));
      this.layers = {};
      for (const [k, v] of entries(get(this.root, "action_layers"))) this.layers[k] = { title: get(v, "title") || k, parent: get(v, "parent_set_name") };
      this.sets = {};
      for (const [k, v] of entries(get(this.root, "actions"))) this.sets[k] = { title: get(v, "title") || k };
      this.loc = {};
      const en = get(get(this.root, "localization"), "english");
      for (const [k, v] of entries(en)) this.loc[k] = v;
      this.notes = [];
    }
    /* action sets and layers in the file */
    list() {
      return this.presets.map(p => {
        const layer = p.name in this.layers;
        let title = layer ? this.layers[p.name].title : (this.sets[p.name] ? this.sets[p.name].title : p.name);
        if (title && title[0] === "#") title = this.loc[title.slice(1)] || title.slice(1);
        return { id: p.id, name: p.name, title: title || p.name, layer, parent: layer ? this.layers[p.name].parent : null };
      });
    }
    /* the game actions the file uses: [{ name, title, def }] - def: the key the game's manifest names, if any */
    gameActions() {
      const used = new Map();
      const walk = nd => { for (const [k, v] of entries(nd)) { if (typeof v === "string") { if (k.toLowerCase() === "binding" && /^game_action\s/i.test(v)) { const p = v.split(",")[0].trim().split(/\s+/), t = v.split(",")[1]; if (p[2] && !used.has(p[2])) used.set(p[2], { name: p[2], title: (t || "").trim() }); } } else walk(v); } };
      for (const g of Object.values(this.groups)) walk(g);
      const ctx = {};
      for (const [, set] of entries(get(this.root, "actions"))) for (const [kind, lst] of entries(set)) {
        if (typeof lst === "string") continue;
        for (const [name, v] of entries(lst)) {
          const a = used.get(name); if (!a) continue;
          const txt = typeof v === "string" ? v : (get(v, "title") || "");
          const parts = txt.split(",");
          if (!a.title) { let t = parts[0].trim(); if (t[0] === "#") t = this.loc[t.slice(1)] || t.slice(1); a.title = t; }
          if (parts[1] && a.def == null) { let b = parts[1].trim().replace(/^key_press ESC$/i, "key_press ESCAPE"); const r = parseBinding(b, ctx); if (r.act != null) a.def = r.act; }
        }
      }
      return [...used.values()].map(a => ({ name: a.name, title: a.title || a.name, def: a.def == null ? 0 : a.def }));
    }
    gameDefaults() { if (!this._gd) { this._gd = {}; for (const a of this.gameActions()) if (a.def) this._gd[a.name] = a.def; } return this._gd; }
    /* the default choice for the 4 slots: the first action set, its layers, then the other sets */
    defaultSlots() {
      const l = this.list(); if (!l.length) return [];
      const first = l.find(x => !x.layer) || l[0];
      const order = [first, ...l.filter(x => x.layer && x.parent === first.name), ...l.filter(x => !x.layer && x !== first), ...l.filter(x => x.layer && x.parent !== first.name)];
      return [...new Set(order)].slice(0, 4).map(x => x.id);
    }
    note(s) { if (!this.notes.includes(s)) this.notes.push(s); }
    group(id) {
      let g = this.groups[id];
      for (let k = 0; g && get(g, "mode") === "reference" && k < 4; k++) g = this.groups[get(get(g, "settings"), "referenced_mode")];
      return g;
    }
    /* one action set / layer -> profile bytes; slotOf: preset id -> profile slot (for switches and layers) */
    profile(p, slotOf) {
      const b = emptyProfile(this.list().find(x => x.id === p.id).title);
      const ctx = { b, slotOf, gameMap: this.opts.gameMap || this.gameDefaults(), padXbox: this.opts.padXbox, usesPad: false, unsupported: {}, game: 0, isLayer: p.name in this.layers };
      const active = [], shifts = [];
      for (const [gid, how] of entries(p.gsb)) {
        const w = String(how).toLowerCase().split(/\s+/), src = w[0];
        if (w.includes("active") && !w.includes("modeshift")) active.push([src, gid]);
        else if (w.includes("modeshift")) shifts.push([src, gid, w.includes("active")]);
      }
      /* a PlayStation layout's touchpad: our right trackpad, when the layout leaves that free */
      const hasRP = active.some(([src]) => src === "right_trackpad");
      for (const x of [...active, ...shifts]) if (x[0] === "center_trackpad") {
        if (hasRP) this.note("touchpad of the other controller: the right trackpad is already used, left out");
        else { x[0] = "right_trackpad"; this.note("touchpad of the other controller: on the right trackpad"); }
      }
      for (const [src, gid] of active) this.applyGroup(ctx, src, this.group(gid), null);
      /* mode shifts: the buttons that shift come from mode_shift bindings found above */
      for (const [src, gid] of shifts) {
        const sg = SOURCES[src]; if (!sg || sg.g == null) continue;
        const btn = ctx.shiftBtn && ctx.shiftBtn[src + " " + gid];
        if (btn == null) continue;
        b[SH_OFF(sg.g)] = btn + 1;
        this.applyGroup(ctx, src, this.group(gid), sg.g);
      }
      /* a shift of the gyro: not available */
      for (const [src, gid] of shifts) if (src === "gyro" && ctx.shiftBtn && ctx.shiftBtn[src + " " + gid] != null) this.note("mode shift of the gyro: not available (the gyro keeps its normal mode)");
      /* settings that change the feel and have no equivalent here: said once, by name */
      {
        const FEEL = { mouse_smoothing: "trackpad smoothing", gyro_precision_speed: "gyro precision speed", gyro_speed_deadzone: "gyro speed dead zone",
          mouse_dampening_trigger: "trigger press mouse dampening", gyro_roll_scale: "gyro roll scale", flickstick_snap_tightness: "flick stick snap",
          flickstick_depression_speed: "flick stick smoothing", friction_vert_scale: "vertical friction", edge_spin_radius: "edge spin radius",
          gyro_to_joystick_out_min: "gyro joystick minimum output", gyro_speed_to_joystick_min: "gyro joystick minimum speed",
          gyro_to_joystick_enable_angle_catchup: "gyro joystick angle catch-up", analog_emulation_period: "D-pad analog emulation",
          gyro_button_haptic_effect: null, haptic_intensity_override: null };
        const seen = new Set();
        for (const [, gid] of active) { const st = get(this.group(gid) || {}, "settings"); if (!st) continue;
          for (const [k, v] of entries(st)) if (FEEL[k] && v !== "0" && v !== "") seen.add(FEEL[k]); }
        if (seen.size) this.note("settings not available here (left at the default feel): " + [...seen].join(", "));
      }
      if (ctx.game) this.note(`${ctx.game} game action binding(s) skipped: they belong to the Steam Input API of that game (Steam alone knows the key behind them)`);
      for (const u of Object.keys(ctx.unsupported)) this.note("skipped: " + u);
      if (ctx.usesPad && this.opts.padXbox) this.note("gamepad outputs go to the Xbox 360 pad over USB with Pad output “Steam + Xbox 360” or “Xbox 360 only”; over Bluetooth, the Puck or USB with the Steam pad only they go to the Steam controller pad (same buttons, sticks, triggers)");
      return b;
    }
    applyGroup(ctx, src, g, shiftG) {
      if (!g) return;
      const sg = SOURCES[src]; if (!sg) { this.note("source " + src + ": not on this controller"); return; }
      const mode = (get(g, "mode") || "").toLowerCase(), set = get(g, "settings"), inputs = get(g, "inputs");
      const S = k => { const v = get(set, k); return v == null ? null : +v; };
      const b = ctx.b;
      const bindAt = (s, inp, opts) => this.bindInput(ctx, s, inp, shiftG, opts || {});
      if (sg.kind === "switch") {
        for (const [name, inp] of entries(inputs)) {
          if (name === "always_on_action") { this.alwaysOn(ctx, inp); continue; }
          if (name in SWITCH) bindAt(SWITCH[name], inp);
          else if (name in DIAMOND) bindAt(DIAMOND[name], inp);
          else if (name === "left_stick_click" || name === "right_stick_click") bindAt(name[0] === "l" ? 15 : 5, inp);
          /* the Steam Controller 2026's triggers as switches: the full pull (click) and the threshold
           * (soft pull) - "Left Trigger Enables Gyro" style layouts shift with them */
          else if (name === "left_trigger" || name === "right_trigger") bindAt(name[0] === "l" ? 27 : 23, inp);
          else if (name === "left_trigger_threshold" || name === "right_trigger_threshold") bindAt(name[0] === "l" ? 58 : 59, inp);
          else this.note("button " + name + ": not on this controller");
        }
        return;
      }
      if (sg.kind === "diamond") {
        if (mode !== "four_buttons" && mode !== "" ) this.note(`face buttons as “${mode}”: used as four buttons`);
        for (const [name, inp] of entries(inputs)) if (name in DIAMOND) bindAt(DIAMOND[name], inp);
        return;
      }
      if (sg.kind === "dpad") {
        const lay = S("layout");
        /* layout 0 four way, 1 eight way; none: the physical D-pad's own (both directions of a diagonal) */
        const four = lay === 0 ? 1 : 0;
        if (shiftG == null) b[16] = four; else b[SH_OFF(1) + 1] = four;
        const D = { dpad_north: 13, dpad_south: 10, dpad_west: 12, dpad_east: 11 };
        for (const [name, inp] of entries(inputs)) if (name in D) bindAt(D[name], inp);
        if (mode !== "dpad" && mode !== "four_buttons") this.note(`D-pad as “${mode}”: used as a D-pad`);
        return;
      }
      if (sg.kind === "trig") {
        const t = sg.t;
        for (const [name, inp] of entries(inputs)) {
          if (name === "click") bindAt(58 + t, inp);          /* soft pull */
          else if (name === "edge") bindAt(t ? 23 : 27, inp);  /* full pull (the click) */
        }
        /* Steam's threshold style (the order of its list): simple, hair trigger, hip fire aggressive /
         * normal / relaxed, hip fire exclusive */
        { const at = S("adaptive_threshold");
          if (at != null && at > 0 && shiftG == null) {
            b[104 + t] = at === 1 ? 1 : at >= 5 ? 3 : 2;
            if (at >= 2 && at <= 4) setU16(b, 106 + 2 * t, [40, 75, 150][at - 2]);
          } }
        const out = S("output_trigger");
        if (out === 1 || out === 2) {
          if (ctx.padXbox) { const s = 58 + t, o = shiftG != null ? SB_OFF(shiftIndex(s)) : B_OFF(s); this.addAct(ctx, o, 0, out === 1 ? 0xDA : 0xDB); ctx.usesPad = true; }
        }
        const dz = S("deadzone_inner_radius");
        if (dz && shiftG == null) setU16(b, 110 + 4 * t, dz > 100 ? dz / 32767 * 10000 : dz * 100);
        if (S("output_trigger") === 0 || mode !== "trigger") { /* nothing */ }
        return;
      }
      if (sg.kind === "gyro") { if (shiftG == null) this.gyro(ctx, mode, set); return; }
      /* sticks and trackpads */
      const i = sg.i, pad = i >= 2, ao = shiftG != null ? SH_OFF(shiftG) + 2 : AN_OFF(i), xo = ANX + 12 * (shiftG != null ? 4 + shiftG : i);
      const base = AN_BASE[i];
      const setMode = m => { b[ao] = m; };
      const inv = () => { if (S("invert_x")) b[ao + 1] |= 0x20; if (S("invert_y")) b[ao + 1] |= 0x40; const r = S("rotation"); if (r) b[ao + 9] = Math.max(-90, Math.min(90, r)) & 255; };
      const sens = (def, k) => { const v = S("sensitivity"); return v == null ? def : def * v / 100; };
      const scales = () => { const h = S("sensitivity_horiz_scale"), v = S("sensitivity_vert_scale"); if (h != null && h !== 100) setU16(b, xo, h); if (v != null && v !== 100) setU16(b, xo + 2, v); };
      const dzIn = (def) => { const v = S("deadzone_inner_radius") ?? S("deadzone"); if (v == null) return def; return v > 100 ? v / 32767 * 10000 : v * 100; };
      switch (mode) {
        case "joystick_move": case "joystick_camera": case "joystick_to_camera_angles": case "mouse_joystick":
          if (mode === "mouse_joystick" && pad) {
            /* Steam's mouse joystick: the finger's speed deflects the stick (firmware v51 test 62+) */
            setMode(14); ctx.usesMouseJoy = true;
            { const sv = S("sensitivity"); setU16(b, ao + 6, sv != null ? Math.max(10, Math.min(1000, sv)) : 100); }
            const mdz = Math.max(S("mousejoystick_deadzone_x") || 0, S("mousejoystick_deadzone_y") || 0);
            if (mdz) setU16(b, ao + 14, Math.min(9000, mdz / 32767 * 10000));
            if (S("trackball") !== 0 && S("friction") != null) { b[ao + 1] |= 0x80; const f = S("friction"); setU16(b, ao + 4, f ? f * 100 : 200); }
            if (ctx.padXbox) { b[ao + 1] |= 8; ctx.usesPad = true; }
            { const o = S("output_joystick"); if (o != null && ((i === 2 && o === 2) || (i === 3 && o === 1))) b[ao + 1] |= 4; }
            scales();
            break;
          }
          if (S("output_joystick") === 3) {
            /* the stick's output set to the mouse */
            setMode(3); setU16(b, ao + 6, sens(pad ? 1200 : 1500)); scales(); break;
          }
          if (mode === "joystick_to_camera_angles") { setMode(3); setU16(b, ao + 6, sens(1500)); const c = curveIn(S("curve_exponent"), S("custom_curve_exponent")); if (c && c !== 100) setU16(b, ao + 10, c); scales(); this.note("camera angles (game action) on a stick: used as stick → mouse"); break; }
          if (pad) { setMode(ctx.padXbox ? 12 : 6); setU16(b, ao + 2, 1000); setU16(b, ao + 4, 1000); }
          else if (ctx.padXbox) { setMode(12); ctx.usesPad = true; }
          else setMode(0);
          if (!pad) {
            /* Steam's dead zones are in stick units (0..32767), ours 0.01 % of the radius */
            const di = S("deadzone_inner_radius"), dout = S("deadzone_outer_radius"), c = curveIn(S("curve_exponent"), S("custom_curve_exponent"));
            if (di != null) setU16(b, ao + 2, Math.min(9000, di / 32767 * 10000));
            if (dout != null && dout < 32767) setU16(b, ao + 4, Math.min(9000, (32767 - dout) / 32767 * 10000));
            if (c && c !== 100) setU16(b, ao + 10, c);
            const ad = S("anti_deadzone"); if (ad) setU16(b, ao + 14, Math.min(9000, ad > 100 ? ad / 32767 * 10000 : ad * 100));
          }
          if (get(g, "gameactions")) this.note(`${AN_SRC[i]}: an analog game action (${entries(get(g, "gameactions")).map(([, v]) => v).join(", ")}): sent as ${ctx.padXbox ? "the Xbox" : "the Steam"} stick`);
          { const o = S("output_joystick"); if (o != null && ((i % 2 === 0 && o === 2) || (i % 2 === 1 && o === 1))) b[ao + 1] |= 4; }
          if (pad && ctx.padXbox) ctx.usesPad = true;
          break;
        case "joystick_mouse":
          setMode(3); setU16(b, ao + 6, sens(pad ? 1200 : 1500)); { const c = curveIn(S("curve_exponent"), S("custom_curve_exponent")); if (c && c !== 100 && !pad) setU16(b, ao + 10, c); } scales(); break;
        case "absolute_mouse": case "touchpad_to_camera_angles":
          if (pad) { setMode(3); setU16(b, ao + 6, sens(1200)); if (S("trackball") === 1 || S("friction") != null && S("trackball") !== 0) { if (S("trackball") !== 0) { b[ao + 1] |= 0x80; const f = S("friction"); setU16(b, ao + 4, f ? f * 100 : 200); } } }
          else { setMode(3); setU16(b, ao + 6, sens(1500)); }
          scales(); { const es = S("edge_spin_velocity"); if (es && pad) setU16(b, xo + 4, es * 10); }
          break;
        case "scrollwheel": case "scroll_wheel":
          setMode(4); if (pad) setU16(b, ao + 6, 30); else setU16(b, ao + 6, 800); break;
        case "dpad":
          setMode(S("layout") === 1 ? 2 : 1); setU16(b, ao + 2, dzIn(pad ? 2000 : 3000));
          if (pad && S("requires_click") !== 0) b[ao + 1] |= 1; /* Steam's default on a trackpad: click to press */
          break;
        case "four_buttons": /* button pad: diamond regions */
          setMode(1); setU16(b, ao + 2, dzIn(pad ? 2000 : 3000)); if (pad && S("requires_click") !== 0) b[ao + 1] |= 1; break;
        case "flickstick": case "flickstick_to_camera_angles":
          setMode(7); setU16(b, ao + 2, 8000); setU16(b, ao + 6, 100); break;
        case "radial_menu": case "touch_menu": case "hotbar_menu": case "hotbar": {
          const n = Math.max(S("touch_menu_button_count") || 0, ...entries(inputs).filter(([k]) => k.startsWith("touch_menu_button_")).map(([k]) => +k.slice(18) + 1), 0);
          setMode(10); setU16(b, ao + 2, pad ? 2500 : 3000); b[ao + 8] = 1;
          if (pad) setU16(b, ao + 6, Math.max(2, Math.min(10, n || 8))); else b[xo + 6] = Math.max(2, Math.min(8, n || 8));
          if (n > (pad ? 10 : 8)) this.note(`${mode.replace(/_/g, " ")} on ${AN_SRC[i]}: ${n} entries, this controller has ${pad ? 10 : 8} slices there: entries ${pad ? 11 : 9}-${n} are left out`);
          if (mode !== "radial_menu") this.note(`${mode.replace("_", " ")} on ${AN_SRC[i]}: used as a radial menu (no on-screen menu: find the slices by feel)`);
          if (S("touchmenu_button_fire_type") === 1 || S("requires_click") === 1) b[ao + 1] |= 1;
          break; }
        case "2dscroll": case "directional_swipe":
          /* Steam's directional swipe: a swipe fires its direction */
          if (pad) { setMode(8); setU16(b, ao + 2, dzIn(2000)); } else this.note(`${AN_SRC[i]} as “${mode}”: not available, left to Steam`);
          break;
        case "single_button":
          setMode(11); break;
        case "mouse_region":
          setMode(13); { const x = S("position_x"), y = S("position_y"), sc = S("scale"); b[xo + 8] = x != null ? x : 50; b[xo + 9] = y != null ? y : 50; b[xo + 10] = sc != null ? Math.max(5, Math.min(100, sc)) : 100; b[xo + 11] = sc != null ? Math.max(5, Math.min(100, sc)) : 100; }
          break;
        default:
          this.note(`${AN_SRC[i]} as “${mode}”: not available, left to Steam`);
      }
      { const own = S("sc26re_mode"); if (own === 0 || own === 5 || own === 8 || own === 9) { setMode(own); } }
      inv();
      /* inputs of the mode */
      const m = b[ao];
      const slices = (() => {
        if (m !== 10) return null;
        if (pad) { const n = u16(b, ao + 6); return (i === 2 ? [38, 47, 41, 49, 39, 48, 40, 46, 64, 54] : [42, 51, 45, 53, 43, 52, 44, 50, 65, 56]).slice(0, n); }
        const d = i === 0 ? 70 : 74, L8 = [base, d, base + 3, d + 1, base + 1, d + 2, base + 2, d + 3], n = b[xo + 6] || 8;
        return n === 4 ? [L8[0], L8[2], L8[4], L8[6]] : L8.slice(0, n);
      })();
      const ring = 60 + i, center = pad ? 64 + i - 2 : null;
      let clickBound = false, touchBound = false;
      for (const [name, inp] of entries(inputs)) {
        const nm = name.toLowerCase();
        if (nm.startsWith("touch_menu_button_")) { const k = +nm.slice(18); if (slices && k < slices.length) bindAt(slices[k], inp); continue; }
        const D = { dpad_north: 0, dpad_south: 1, dpad_west: 2, dpad_east: 3, button_y: 0, button_a: 1, button_x: 2, button_b: 3 };
        if (nm in D) { bindAt(base + D[nm], inp); continue; }
        if (nm === "scroll_clockwise" || nm === "scroll_wheel_clockwise") { if (pad) bindAt(i === 2 ? 54 : 56, inp); continue; }
        if (nm === "scroll_counterclockwise" || nm === "scroll_wheel_counterclockwise") { if (pad) bindAt(i === 2 ? 55 : 57, inp); continue; }
        if (nm === "edge") { bindAt(ring, inp); if (!u16(b, ao + 12)) { const r = S("edge_binding_radius"); setU16(b, ao + 12, r && r > 1000 && r <= 32767 ? r / 32767 * 10000 : 9000); } continue; }
        if (nm === "click") {
          /* a soft press: on a single-button pad the pad's pressure fires it; elsewhere the click does */
          const soft = () => { if (pad && m === 11) { b[ao + 1] |= 0x10; setU16(b, ao + 16, 3000); } else if (pad) this.note(`${AN_SRC[i]}: soft press on the click: the full click is used`); };
          if (m === 11) { if (pad) { if (bindAt(center, inp, { soft }) && !(b[ao + 1] & 0x10)) b[ao + 1] |= 1; } else bindAt(ring, inp); clickBound = true; continue; }
          bindAt(pad ? (i === 2 ? 26 : 22) : (i === 0 ? 15 : 5), inp, { soft }); clickBound = true; continue;
        }
        if (nm === "touch") {
          if (m === 11 && pad && !clickBound) { bindAt(center, inp); touchBound = true; continue; }
          bindAt(pad ? (i === 2 ? 25 : 21) : (i === 0 ? 24 : 20), inp); touchBound = true; continue;
        }
        this.note(`${AN_SRC[i]} input ${name}: not available`);
      }
      if (m === 11 && !pad) { /* single button stick: past the dead zone */ setU16(b, ao + 2, 3000); }
    }
    hasActivator(inp, k) { return entries(get(inp, "activators")).some(([a]) => a.toLowerCase() === k); }
    addAct(ctx, o, g, a) { const b = ctx.b; for (let k = 0; k < 4; k++) { if (b[o + 1 + g * 4 + k] === a) return; if (!b[o + 1 + g * 4 + k]) { b[o + 1 + g * 4 + k] = a; return; } } ctx.unsupported["more than 4 actions on one input (extra dropped)"] = 1; }
    alwaysOn(ctx, inp) {
      for (const [, act] of entries(get(inp, "activators"))) for (const bs of all(get(act, "bindings"), "binding")) {
        const r = parseBinding(bs, ctx);
        if (r.led) { ctx.b[40] = Math.min(255, r.led[0]); ctx.b[41] = Math.min(255, r.led[1]); ctx.b[42] = Math.min(255, r.led[2]); if (!(ctx.b[40] | ctx.b[41] | ctx.b[42])) ctx.b[42] = 1; }
      }
    }
    /* the activators of one input -> the binding of source s */
    bindInput(ctx, s, inp, shiftG, opts) {
      const b = ctx.b;
      let o, bt;
      if (shiftG != null) {
        const k = shiftIndex(s), c = SCLICK_SRC.indexOf(s);
        if (k >= 0) { o = SB_OFF(k); bt = BT + 6 * btIndex(s, k); }
        else if (c >= 0) { o = SCLICK + 14 * c; bt = SCLICK_BT + 6 * c; } /* the stick / pad click of a mode shift (v8) */
        else { this.note("shifted input " + s + ": not available"); return 0; }
      }
      else { o = B_OFF(s); bt = BT + 6 * btIndex(s); }
      let bound = 0, regular = null;
      for (const [aname, act] of entries(get(inp, "activators"))) {
        const an = aname.toLowerCase(), set = get(act, "settings"), S = k => { const v = get(set, k); return v == null ? null : +v; };
        let g = 0;
        if (an === "long_press") g = 1;
        else if (an === "double_press" || an === "release") g = 2;
        else if (an === "full_press" || an === "soft_press" || an === "start_press" || an === "chorded_press" || an === "chord") g = 0;
        else if (an === "analog") { ctx.unsupported["analog activator"] = 1; continue; }
        else { ctx.unsupported["activator " + aname] = 1; continue; }
        /* the outputs first: an activator with nothing this controller can send leaves no trace */
        const acts = [];
        for (const bs of all(get(act, "bindings"), "binding")) {
          const r = parseBinding(bs, ctx);
          if (r.act != null) acts.push(r.act);
          else if (r.modeShift) {
            ctx.shiftBtn = ctx.shiftBtn || {};
            /* a button, or a trigger's soft pull (shift button 31 / 32 in the firmware, v51 test 61+) */
            const sb = s < 30 ? s : s === 58 ? 30 : s === 59 ? 31 : null;
            if (sb != null && shiftG == null) { ctx.shiftBtn[r.modeShift.source + " " + r.modeShift.group] = sb; bound++; }
            else this.note("mode shift from a stick, pad or shifted input: only a plain button can shift");
          }
          else if (r.profile != null) {
            const slot = ctx.slotOf(r.profile);
            if (slot == null) this.note(`switch to action set ${r.profile + 1}: that set was not imported`);
            else acts.push(0xC0 + slot);
          }
          else if (r.layer != null) {
            const slot = ctx.slotOf(r.layer);
            if (slot == null) this.note(`layer ${r.layer + 1}: that layer was not imported`);
            else { acts.push(0xC4 + slot); if (r.approx) this.note(r.approx); }
          }
          else if (r.led) { b[40] = Math.min(255, r.led[0]); b[41] = Math.min(255, r.led[1]); b[42] = Math.min(255, r.led[2]); }
          else if (r.game) ctx.game++;
          else if (r.unsupported) ctx.unsupported[r.unsupported] = 1;
        }
        if (!acts.length) continue;
        bound += acts.length;
        if (an === "long_press") { b[o] |= 1; const t = S("long_press_time") || STEAM_LONG_MS; b[bt] = Math.max(1, Math.min(255, Math.round(t / 10))); }
        else if (an === "double_press") { if (b[o] & 16) { this.note("double press and release press on one input: one slot here, the double press kept"); b.fill(0, o + 9, o + 13); } b[o] = (b[o] | 2) & ~16; const t = S("double_tap_time"); if (t) b[bt + 1] = Math.min(255, Math.round(t / 10)); }
        else if (an === "release") { if (b[o] & 2) { this.note("double press and release press on one input: one slot here, the double press kept"); continue; } b[o] |= 16; }
        else if (an === "start_press") b[o] = (b[o] | 8) & ~4;
        else if (an === "chorded_press" || an === "chord") { const cb = get(set, "chord_button"); const bit = cb != null ? (SWITCH[cb] ?? DIAMOND[cb] ?? null) : null; if (bit != null) b[o + 13] = (b[o + 13] & 3) | ((bit + 1) << 2); else ctx.unsupported["chorded press without a known chord button"] = 1; }
        else if (an === "soft_press" && opts.soft) opts.soft();
        if (g !== 0 && (S("toggle") === 1 || S("hold_repeats") === 1 || S("repeat") === 1 || S("turbo") === 1)) this.note("toggle / turbo on a long, double or release press: here only on the regular press (left out)");
        if (g === 0 && S("toggle") === 1) b[o] = (b[o] | 4) & ~8;
        if (g === 0 && (S("hold_repeats") === 1 || S("repeat") === 1 || S("turbo") === 1)) { b[o] |= 32; const r = S("repeat_rate"); if (r) b[bt + 2] = Math.min(255, Math.max(4, Math.round(r / 5))); }
        if (S("cycle") === 1) b[o] |= 64;
        const ds = S("delay_start"), de = S("delay_end");
        if (ds) b[bt + 3] = Math.min(255, Math.round(ds / 10));
        if (de) b[bt + 4] = Math.min(255, Math.round(de / 10));
        const hp = S("haptic_intensity"); if (hp != null) b[o + 13] = (b[o + 13] & ~3) | Math.max(0, Math.min(3, hp));
        for (const a of acts) this.addAct(ctx, o, g, a);
        if (an === "full_press" || an === "soft_press") regular = regular || (S("interruptable") === 1 || S("interruptible") === 1 ? "int" : "keep");
      }
      /* Steam: a regular press next to a long / double press acts at once unless it is
       * "interruptible" (then a short tap fires it when let go, as our long press does) */
      if (regular === "keep" && (b[o] & 3) && !(b[o] & 0x0C)) b[bt + 5] |= 2;
      return bound;
    }
    gyro(ctx, mode, set) {
      const S = k => { const v = get(set, k); return v == null ? null : +v; };
      const b = ctx.b, G = (k, v) => setS16(b, 44 + 2 * k, v);
      const keys = ["gm_output", "gm_act_button", "gm_act_mode", "gm_axis_pitch", "gm_axis_yaw", "gm_axis_roll", "gm_sens", "gm_scale_p", "gm_scale_y", "gm_scale_r", "gm_curve", "gm_accel", "gm_accel_lo", "gm_accel_hi", "gj_dz", "gj_anti", "gj_max", "gj_curve", "gj_tilt", "gm_haptic", "gm_qt1_btn", "gm_qt1_deg", "gm_qt2_btn", "gm_qt2_deg", "gm_qt3_btn", "gm_qt3_deg", "gm_qt4_btn", "gm_qt4_deg", "gm_qt_ms", "gm_ratchet"];
      const K = n => keys.indexOf(n);
      const def = { gm_output: 0, gm_act_button: -1, gm_act_mode: 0, gm_axis_pitch: 2, gm_axis_yaw: 1, gm_axis_roll: 0, gm_sens: 100, gm_scale_p: 100, gm_scale_y: 100, gm_scale_r: 100, gm_curve: 10, gm_accel: 0, gm_accel_lo: 0, gm_accel_hi: 200, gj_dz: 0, gj_anti: 0, gj_max: 300, gj_curve: 100, gj_tilt: 30, gm_haptic: 0, gm_qt1_btn: -1, gm_qt1_deg: 0, gm_qt2_btn: -1, gm_qt2_deg: 0, gm_qt3_btn: -1, gm_qt3_deg: 0, gm_qt4_btn: -1, gm_qt4_deg: 0, gm_qt_ms: 60, gm_ratchet: -1 };
      /* the gyro's legacy modes: absolute_mouse = as mouse, mouse_joystick / joystick_camera = as a
       * camera joystick, joystick_move = joystick deflection */
      const toMouse = ["gyro_to_mouse", "gyro_to_camera_angles", "absolute_mouse"].includes(mode);
      const toJoy = ["gyro_to_joystick", "gyro_to_joystick_camera", "joystick_camera", "mouse_joystick"].includes(mode), toDefl = mode === "gyro_to_joystick_deflection" || mode === "joystick_deflection" || mode === "joystick_move";
      if (!toMouse && !toJoy && !toDefl) { if (mode) this.note(`gyro as “${mode}”: not available`); return; }
      for (const [k, v] of Object.entries(def)) G(K(k), v);
      b[17] |= 2 | 4; /* own gyro settings, gyro on with this profile */
      G(K("gm_output"), (toMouse ? 0 : toJoy ? 1 : 3) + (!toMouse && S("output_joystick") === 1 ? 1 : 0)); /* 1 = Steam's left stick */
      const ns = S("gyro_natural_sensitivity"); if (ns) G(K("gm_sens"), ns);
      else if (S("sensitivity")) G(K("gm_sens"), S("sensitivity"));
      const conv = S("gyro_to_2d_conversion_style");
      if (conv != null) {
        if (conv >= 6) b[GYX + 1] = 3; else if (conv >= 4) b[GYX + 1] = 2;
        else { b[GYX + 1] = 1; if (conv === 2) { G(K("gm_axis_yaw"), 0); G(K("gm_axis_roll"), 1); } else if (conv === 3) G(K("gm_axis_roll"), 1); }
        this.note("gyro “3D to 2D” style mapped to " + ["", "local space", "player space", "world space"][b[GYX + 1]]);
      }
      if (S("invert_y")) G(K("gm_axis_pitch"), -2);
      if (S("invert_x")) G(K("gm_axis_yaw"), -1);
      const gb = get(set, "gyro_button"), inv = S("gyro_button_invert");
      const btn = gb != null ? (SWITCH[gb] ?? DIAMOND[gb] ?? null) : null;
      if (btn != null) { G(K("gm_act_button"), btn); G(K("gm_act_mode"), inv === 1 ? 2 : inv === 2 ? 3 : 1); }
      else if (gb != null && gb !== "0" && gb !== "1") this.note("gyro activation button “" + gb + "”: not recognised, the gyro is always on");
      {
        /* Steam's gyro activation buttons as a mask: the Steam Controller's button bits (0 RT, 1 LT, 2 RB,
         * 3 LB, 4 Y, 5 B, 6 X, 7 A, 8..11 D-pad up / right / left / down, 12 View, 13 Steam, 14 Menu,
         * 15 / 16 the lower back buttons, 17 / 18 pad clicks, 19 / 20 pad touches, 22 L3; on the newer
         * controllers 46 / 47 the stick touches). The firmware takes one button: the first one known */
        const mstr = get(set, "gyro_ratchet_button_mask");
        if (mstr != null && mstr !== "0") {
          const MB = GYRO_MASK_BTN;
          let m = BigInt(mstr), bits = [];
          for (let i = 0; m; i++, m >>= 1n) if (m & 1n) bits.push(i);
          const known = bits.filter(i => MB[i] != null), unknown = bits.filter(i => MB[i] == null);
          if (known.length) {
            /* all of them in the profile's activation mask (firmware v51 test 62+); the first plain
             * button also as gm_act_button for older firmware */
            let am = 0, af = S("gyro_ratchet_button_requireany_or_all") === 1 ? 1 : 0;
            for (const k of known) { const v = MB[k]; if (v < 30) am |= 1 << v; else if (v === 130) am |= 1 << 30; else if (v === 131) am |= 1 << 31; else af |= v === 140 ? 2 : 4; }
            b[GYX + 9] = am & 255; b[GYX + 10] = am >>> 8 & 255; b[GYX + 11] = am >>> 16 & 255; b[GYX + 12] = am >>> 24 & 255; b[GYX + 13] = af;
            const plain = known.find(k => MB[k] < 30);
            if (plain != null) G(K("gm_act_button"), MB[plain]);
            /* Steam: no gyro_button_invert = on while held, "0" = off while held, "2" = toggle */
            const inv2 = get(set, "gyro_button_invert");
            G(K("gm_act_mode"), inv2 === "0" || inv2 === "1" ? 2 : inv2 === "2" ? 3 : 1);
            if (unknown.length) this.note(`gyro activation: input(s) ${unknown.join(", ")} of Steam's mask are not on this controller, left out`);
          } else this.note("gyro activation buttons: none this controller knows (mask " + mstr + "), the gyro is on all the time");
        }
      }
      { const r = S("gyro_vertical_horizontal_ratio"); if (r != null && r !== 100) G(K("gm_scale_p"), Math.max(0, Math.min(400, r))); }
      if (S("acceleration")) G(K("gm_accel"), 50);
      if (conv == null && S("invert_x") == null && S("invert_y") == null) { /* nothing */ }
      if (S("haptic_intensity")) G(K("gm_haptic"), Math.min(3, S("haptic_intensity")));
    }
    run() {
      const lst = this.list(), slots = this.opts.slots || this.defaultSlots();
      const slotOf = id => { const k = slots.indexOf(id); return k < 0 ? null : k; };
      const profiles = [];
      for (let k = 0; k < slots.length; k++) {
        const p = this.presets.find(x => x.id === slots[k]);
        profiles.push(p ? this.profile(p, slotOf) : null);
      }
      return { profiles, notes: this.notes, list: lst, slots, layers: slots.map(id => { const x = lst.find(y => y.id === id); return !!(x && x.layer); }) };
    }
  }

  /* ---------------- export ---------------- */
  function actString(a) {
    if (a >= 0x04 && a <= 0x65) return KEYNAME[a] ? `key_press ${KEYNAME[a]}, , ` : null;
    if (a >= 0xE0 && a <= 0xE7) return `key_press ${KEYNAME[a]}, , `;
    if (a in MOUSENAME) return `mouse_button ${MOUSENAME[a]}, , `;
    if (a in WHEELNAME) return `mouse_wheel ${WHEELNAME[a]}, , `;
    if (a >= 0x80 && a < 0xBE) { const bit = (a - 0x80) % 32; return bit in PADNAME ? `xinput_button ${PADNAME[bit]}, , ` : null; }
    if (a >= 0xC0 && a < 0xC4) return `controller_action CHANGE_PRESET ${a - 0xC0 + 1} 0 0, , `;
    if (a >= 0xC4 && a < 0xC8) return `controller_action hold_layer ${a - 0xC4 + 1} 0 0, , `;
    if (a === 0xDA || a === 0xDB) return ""; /* analog trigger output: a setting of the trigger group */
    return null;
  }
  function exportVDF(profiles, opts) {
    opts = opts || {};
    const groups = [], presets = [], dropped = new Set();
    let gid = 0;
    const addGroup = (mode, inputs, settings) => { const id = String(gid++); const g = N(["id", id], ["mode", mode], ["name", ""], ["description", ""], ["inputs", N(...inputs)]); if (settings && settings.length) g.list.push(["settings", N(...settings)]); groups.push(g); return id; };
    const layerOf = new Set(); /* profiles used as layers */
    profiles.forEach(b => { if (!b) return; for (let o = 488; o < 488 + 14 * 112; o += 14) for (let k = 1; k < 13; k++) { const a = b[o + k]; if (a >= 0xC4 && a < 0xC8) layerOf.add(a - 0xC4); } });
    const isLayer = j => opts.layers ? !!opts.layers[j] : layerOf.has(j);
    const inputOf = (b, o, bt, extra) => {
      const m = b[o], acts = g => [1, 2, 3, 4].map(k => b[o + g * 4 + k]).filter(Boolean);
      const L = [];
      const mk = (name, g, settings) => {
        const bs = []; for (const a of acts(g)) { if (a >= 0xC0 && a < 0xC8 && !profiles[(a - 0xC0) & 3]) { dropped.add("switches to an empty profile"); continue; }
          if (a >= 0xC0 && a < 0xC4 && isLayer(a - 0xC0)) { dropped.add("a profile switch to a profile used as a layer (Steam: a layer is not an action set)"); continue; }
          if (a >= 0xC4 && a < 0xC8 && !isLayer(a - 0xC4)) { dropped.add("a layer that is exported as an action set"); continue; } const s = actString(a); if (s) bs.push(["binding", s]); else if (s !== "") dropped.add(a >= 0xC8 && a < 0xCC ? "macros" : a >= 0xD0 && a < 0xDA ? "analog output actions" : a >= 0x76 && a <= 0x79 ? "mouse move actions" : "action 0x" + a.toString(16)); }
        if (!bs.length) return;
        const st = []; const hp = b[o + 13] & 3; if (hp) st.push(["haptic_intensity", String(hp)]);
        for (const x of settings || []) st.push(x);
        const node = N(["bindings", N(...bs)]); if (st.length) node.list.push(["settings", N(...st)]);
        L.push([name, node]);
      };
      const T = []; if (m & 4) T.push(["toggle", "1"]); if (m & 32) { T.push(["hold_repeats", "1"]); if (b[bt + 2]) T.push(["repeat_rate", String(b[bt + 2] * 5)]); }
      if (b[bt + 3]) T.push(["delay_start", String(b[bt + 3] * 10)]); if (b[bt + 4]) T.push(["delay_end", String(b[bt + 4] * 10)]);
      if (m & 64) T.push(["cycle", "1"]);
      if (m & 3 && !(m & 0x0C)) T.push(["interruptable", b[bt + 5] & 2 ? "0" : "1"]);
      const ch = b[o + 13] >> 2, chName = ch ? (SWITCHNAME[ch - 1] || Object.keys(DIAMOND).find(k => DIAMOND[k] === ch - 1)) : null;
      if (chName) mk("Chorded_Press", 0, [...T, ["chord_button", chName]]);
      else mk(m & 8 ? "Start_Press" : (extra && extra.soft ? "Soft_Press" : "Full_Press"), 0, T);
      if (m & 1) mk("Long_Press", 1, [["long_press_time", String(b[bt] ? b[bt] * 10 : (u16(b, 118) || 250))]]); /* always: Steam's default differs from ours */
      if ((m & 1) && (b[bt + 5] & 1)) dropped.add("“another input cancels” on long presses");
      if (m & 2) mk("Double_Press", 2, b[bt + 1] ? [["double_tap_time", String(b[bt + 1] * 10)]] : []);
      if (m & 16) mk("release", 2, []);
      if (ch && !chName) dropped.add("chords on a button Steam cannot chord with");
      if (!L.length) return null;
      return N(["activators", N(...L)], ["disabled_activators", N()]);
    };
    const bindSrc = (b, s, shiftK, extra) => shiftK != null && shiftK < 0 ? null
      : shiftK != null && shiftK >= 1000 ? inputOf(b, SCLICK + 14 * (shiftK - 1000), SCLICK_BT + 6 * (shiftK - 1000), extra)
      : inputOf(b, shiftK != null ? SB_OFF(shiftK) : B_OFF(s), BT + 6 * btIndex(s, shiftK), extra);
    const analogGroup = (b, i, shiftG) => {
      const ao = shiftG != null ? SH_OFF(shiftG) + 2 : AN_OFF(i), xo = ANX + 12 * (shiftG != null ? 4 + shiftG : i), m = b[ao], pad = i >= 2, base = AN_BASE[i];
      /* shifted: index in SHIFT_SRC, or 1000 + index of a v8 shifted click */
      const SK = s => shiftG == null ? null : SHIFT_SRC.indexOf(s) >= 0 ? SHIFT_SRC.indexOf(s) : SCLICK_SRC.indexOf(s) >= 0 ? 1000 + SCLICK_SRC.indexOf(s) : -1;
      const inp = [], set = [];
      const add = (name, s, extra) => { const k = SK(s); if (shiftG != null && k < 0) return; const n = bindSrc(b, s, k, extra); if (n) inp.push([name, n]); };
      let mode = "joystick_move";
      const sp = u16(b, ao + 6);
      /* Steam's joystick_move on a trackpad = the finger position is the stick (ours 6 / 12);
       * its mouse_joystick = the finger's speed is the stick (ours 14) */
      if (m === 0 || m === 12) mode = pad ? (m === 12 ? "joystick_move" : "absolute_mouse") : "joystick_move";
      if (m === 6) mode = "joystick_move";
      if (m === 14) {
        mode = "mouse_joystick"; set.push(["sensitivity", String(sp || 100)]);
        if (u16(b, ao + 14)) { const v = String(Math.round(u16(b, ao + 14) / 10000 * 32767)); set.push(["mousejoystick_deadzone_x", v], ["mousejoystick_deadzone_y", v]); }
        if (b[ao + 1] & 0x80) { set.push(["trackball", "1"]); set.push(["friction", String(Math.round((u16(b, ao + 4) || 200) / 100))]); } else set.push(["trackball", "0"]);
      }
      if (m === 3) { mode = pad ? "absolute_mouse" : "joystick_mouse"; set.push(["sensitivity", String(Math.round(sp / (pad ? 12 : 15)))]); if (pad) { if (b[ao + 1] & 0x80) { set.push(["trackball", "1"]); set.push(["friction", String(Math.round((u16(b, ao + 4) || 200) / 100))]); } else set.push(["trackball", "0"]); } }
      if (m === 4) mode = "scrollwheel";
      if (m === 1 || m === 2) { mode = "dpad"; set.push(["layout", m === 2 ? "1" : "0"]); }
      if (m === 5 || m === 9) { mode = "dpad"; dropped.add(m === 5 ? "trackpad gates (as D-pad)" : "trackpad 9-zone grid (as D-pad)"); }
      if (m === 8) mode = pad ? "2dscroll" : "dpad"; /* Steam's directional swipe */
      if (m === 7) mode = "flickstick";
      if (m === 10) mode = "radial_menu";
      if (m === 11) mode = "single_button";
      if (m === 13) { mode = "mouse_region"; set.push(["position_x", String(b[xo + 8] || 50)], ["position_y", String(b[xo + 9] || 50)], ["scale", String(b[xo + 10] || 100)]); }
      if (m === 3 && pad && u16(b, xo + 4)) set.push(["edge_spin_velocity", String(Math.round(u16(b, xo + 4) / 10))]);
      /* our modes Steam has no name for (or names the same): kept for a round trip, Steam ignores the key */
      if (m === 0 || m === 5 || m === 8 || m === 9) set.push(["sc26re_mode", String(m)]);
      if ((m === 0 || m === 12 || m === 6 || m === 14) && (b[ao + 1] & 4)) set.push(["output_joystick", i % 2 === 0 ? "2" : "1"]);
      if (b[ao + 1] & 0x20) set.push(["invert_x", "1"]); if (b[ao + 1] & 0x40) set.push(["invert_y", "1"]);
      if (u16(b, xo) && (m === 3 || m === 14)) set.push(["sensitivity_horiz_scale", String(u16(b, xo))]); if (u16(b, xo + 2) && (m === 3 || m === 14)) set.push(["sensitivity_vert_scale", String(u16(b, xo + 2))]);
      if (pad && (m === 1 || m === 2 || m === 5 || m === 8 || m === 9 || m === 10)) set.push(["requires_click", b[ao + 1] & 1 ? "1" : "0"]);
      else if (pad && (b[ao + 1] & 1) && m !== 0) set.push(["requires_click", "1"]);
      if (!pad && (m === 0 || m === 12)) {
        if (u16(b, ao + 2)) set.push(["deadzone_inner_radius", String(Math.round(u16(b, ao + 2) / 10000 * 32767))]);
        if (u16(b, ao + 4)) set.push(["deadzone_outer_radius", String(Math.round(32767 - u16(b, ao + 4) / 10000 * 32767))]);
      }
      if (!pad && (m === 0 || m === 12 || m === 3) && u16(b, ao + 10) && u16(b, ao + 10) !== 100) set.push(["curve_exponent", String(curveOut(u16(b, ao + 10)))]);
      if (m === 1 || m === 2 || m === 5 || m === 9 || m === 8) { add("dpad_north", base); add("dpad_south", base + 1); add("dpad_west", base + 2); add("dpad_east", base + 3); }
      if (m === 10) {
        let list;
        if (pad) list = (i === 2 ? [38, 47, 41, 49, 39, 48, 40, 46, 64, 54] : [42, 51, 45, 53, 43, 52, 44, 50, 65, 56]).slice(0, Math.max(2, Math.min(10, sp || 8)));
        else { const d = i === 0 ? 70 : 74, L8 = [base, d, base + 3, d + 1, base + 1, d + 2, base + 2, d + 3], n = b[xo + 6] || 8; list = n === 4 ? [L8[0], L8[2], L8[4], L8[6]] : L8.slice(0, n); }
        list.forEach((s, k) => add("touch_menu_button_" + k, s));
        set.push(["touch_menu_button_count", String(list.length)]);
      }
      if (m === 4 && pad) { add("scroll_clockwise", i === 2 ? 54 : 56); add("scroll_counterclockwise", i === 2 ? 55 : 57); }
      const soft = pad && (b[ao + 1] & 0x10) ? { soft: 1 } : null;
      if (m === 11) add("click", pad ? 64 + i - 2 : 60 + i, soft);
      else add("click", pad ? (i === 2 ? 26 : 22) : (i === 0 ? 15 : 5), soft);
      if (pad) add("touch", i === 2 ? 25 : 21);
      if (m !== 11 && u16(b, ao + 12)) { add("edge", 60 + i); set.push(["edge_binding_radius", String(Math.round(u16(b, ao + 12) / 10000 * 32767))]); }
      if (m === 0 && (pad || layer) && !inp.length) return null;
      return addGroup(mode, inp, set);
    };
    let layer = false;
    const presetFor = (b, name, pid) => {
      const gsb = [];
      const sw = [];
      for (const [nm, bit] of Object.entries(SWITCH)) { const n = bindSrc(b, bit, null); if (n) sw.push([nm, n]); }
      if (b[40] | b[41] | b[42]) sw.push(["always_on_action", N(["activators", N(["Full_Press", N(["bindings", N(["binding", `controller_action set_led ${b[40]} ${b[41]} ${b[42]} 100 100 1, , `])])])], ["disabled_activators", N()])]);
      /* mode shift buttons: add the mode_shift bindings to their inputs (after the groups exist) */
      const shiftGroups = [];
      const groupFor = (src, g) => {
        if (src === "button_diamond") { const inp = []; for (const [nm, bit] of Object.entries(DIAMOND)) { const n = bindSrc(b, bit, g != null ? SHIFT_SRC.indexOf(bit) : null); if (n) inp.push([nm, n]); } if (layer && !inp.length) return null; return addGroup("four_buttons", inp, []); }
        if (src === "dpad") { const inp = []; for (const [nm, bit] of [["dpad_north", 13], ["dpad_south", 10], ["dpad_west", 12], ["dpad_east", 11]]) { const n = bindSrc(b, bit, g != null ? SHIFT_SRC.indexOf(bit) : null); if (n) inp.push([nm, n]); } if (layer && !inp.length) return null; return addGroup("dpad", inp, [["layout", (g != null ? b[SH_OFF(1) + 1] : b[16]) ? "0" : "1"]]); }
        if (src.endsWith("trigger")) { const t = src === "left_trigger" ? 0 : 1, inp = []; const c = bindSrc(b, 58 + t, g != null ? SHIFT_SRC.indexOf(58 + t) : null); if (c) inp.push(["click", c]); const e = bindSrc(b, t ? 23 : 27, g != null ? SHIFT_SRC.indexOf(t ? 23 : 27) : null); if (e) inp.push(["edge", e]); const set = [], o2 = s2 => g != null ? (SHIFT_SRC.indexOf(s2) < 0 ? -1 : SB_OFF(SHIFT_SRC.indexOf(s2))) : B_OFF(s2);
          for (const s2 of [58 + t, t ? 23 : 27]) { const oo = o2(s2); if (oo < 0) continue; for (let k = 1; k < 13; k++) { if (b[oo + k] === 0xDA) set.push(["output_trigger", "1"]); if (b[oo + k] === 0xDB) set.push(["output_trigger", "2"]); } }
          const outs = set.slice(0, 1);
          if (g == null && b[104 + t]) { const tm = b[104 + t], hip = u16(b, 106 + 2 * t) || 75; outs.push(["adaptive_threshold", String(tm === 1 ? 1 : tm === 3 ? 5 : hip <= 55 ? 2 : hip <= 110 ? 3 : 4)]); }
          if (layer && !inp.length && !outs.length) return null;
          return addGroup("trigger", inp, outs); }
        const i = AN_SRC.indexOf(src); return analogGroup(b, i, g);
      };
      const srcs = ["button_diamond", "dpad", "joystick", "right_joystick", "left_trackpad", "right_trackpad", "left_trigger", "right_trigger"];
      const swId = String(gid); /* switches first so mode shifts can be added to it */
      const swNode = N(); const swGroupId = addGroup("switches", [], []); groups[groups.length - 1].list.find(([k]) => k === "inputs")[1] = swNode;
      gsb.push([swGroupId, "switch active"]);
      const baseIds = {};
      srcs.forEach((src, k) => {
        const id = groupFor(src, null); if (id != null) { gsb.push([id, src + " active"]); baseIds[src] = id; }
        const sbt = b[SH_OFF(k)];
        if (sbt) {
          const sid = groupFor(src, k);
          if (sid != null) { gsb.push([sid, src + " inactive modeshift"]); shiftGroups.push([sbt - 1, src, sid]); }
        }
      });
      for (const [nm, n] of sw) swNode.list.push([nm, n]);
      /* the shift buttons: mode_shift bindings on their inputs (switch or diamond) */
      for (const [bit, src, sid] of shiftGroups) {
        let host = swNode, name = SWITCHNAME[bit];
        if (!name) {
          const T = { 0: ["button_diamond", "button_a"], 1: ["button_diamond", "button_b"], 2: ["button_diamond", "button_x"], 3: ["button_diamond", "button_y"],
            13: ["dpad", "dpad_north"], 10: ["dpad", "dpad_south"], 12: ["dpad", "dpad_west"], 11: ["dpad", "dpad_east"], 15: ["joystick", "click"], 5: ["right_joystick", "click"],
            26: ["left_trackpad", "click"], 22: ["right_trackpad", "click"], 27: ["left_trigger", "edge"], 23: ["right_trigger", "edge"],
            30: ["left_trigger", "click"], 31: ["right_trigger", "click"] }[bit];
          const gnode = T && baseIds[T[0]] != null ? groups.find(gr => get(gr, "id") === baseIds[T[0]]) : null;
          if (!gnode) { dropped.add("mode shift on a button Steam cannot shift with"); continue; }
          host = get(gnode, "inputs"); name = T[1];
        }
        let inp = host.list.find(([k]) => k === name);
        if (!inp) { inp = [name, N(["activators", N()], ["disabled_activators", N()])]; host.list.push(inp); }
        get(inp[1], "activators").list.push(["Full_Press", N(["bindings", N(["binding", `mode_shift ${src} ${sid}`])], ["settings", N(["interruptable", "0"])])]);
      }
      /* gyro */
      if (b[17] & 2) {
        const g = k => s16(b, 44 + 2 * k), out = g(0);
        const set = [["gyro_natural_sensitivity", String(g(6) || 100)]];
        const sp = b[GYX + 1]; if (sp) set.push(["gyro_to_2d_conversion_style", String(sp === 3 ? 6 : sp === 2 ? 4 : g(4) === 0 && g(5) === 1 ? 2 : g(5) === 1 ? 3 : 1)]);
        if (g(11)) set.push(["acceleration", "2"]);
        if (g(19)) set.push(["haptic_intensity", String(g(19))]);
        if (g(3) < 0) set.push(["invert_y", "1"]); if (g(4) < 0) set.push(["invert_x", "1"]);
        const ab = g(1);
        const am = (b[GYX + 9] | b[GYX + 10] << 8 | b[GYX + 11] << 16 | b[GYX + 12] << 24) >>> 0, af = b[GYX + 13];
        if (g(7) !== 100 && g(7) > 0) set.push(["gyro_vertical_horizontal_ratio", String(g(7))]);
        if (out === 2 || out === 4) set.push(["output_joystick", "1"]);
        if (am || (af & 6)) {
          /* the profile's activation inputs as Steam's mask, any / all, and the mode */
          let m = 0n;
          for (let v = 0; v < 32; v++) if (am >>> v & 1) { const k = v === 30 ? 25 : v === 31 ? 24 : GYRO_MASK_BIT[v]; if (k != null) m |= 1n << BigInt(k); }
          if (af & 2) m |= 1n << 30n; if (af & 4) m |= 1n << 31n;
          set.push(["gyro_ratchet_button_mask", String(m)]);
          if (af & 1) set.push(["gyro_ratchet_button_requireany_or_all", "1"]);
          if (g(2) === 2) set.push(["gyro_button_invert", "0"]); else if (g(2) === 3) set.push(["gyro_button_invert", "2"]);
        }
        else if (ab >= 0 && SWITCHNAME[ab]) { set.push(["gyro_button", SWITCHNAME[ab]]); set.push(["gyro_button_invert", String(g(2) === 2 ? 1 : g(2) === 3 ? 2 : 0)]); }
        else if (ab >= 0 && GYRO_MASK_BIT[ab] != null && g(2) === 1) set.push(["gyro_ratchet_button_mask", String(1n << BigInt(GYRO_MASK_BIT[ab]))]); /* held: Steam's mask */
        const id = addGroup(out === 0 ? "gyro_to_mouse" : out <= 2 ? "gyro_to_joystick_camera" : "gyro_to_joystick_deflection", [], set);
        gsb.push([id, "gyro active"]);
      }
      if (b[GYX]) dropped.add("gyro tilt / mouse region (no Steam equivalent)");
      return N(["id", String(pid)], ["name", name], ["group_source_bindings", N(...gsb)]);
    };
    const actions = [], layersNode = [], loc = [];
    const firstSet = profiles.findIndex((x, j) => x && !isLayer(j));
    const setKey = j => j === firstSet ? "Default" : "Preset_" + (1000000 + j);
    profiles.forEach((b, k) => {
      if (!b) return;
      const nm = new TextDecoder().decode(b.slice(2, 16)).replace(/\0.*$/, "") || "Profile " + (k + 1);
      /* Steam's own names for a layout without game actions: the first set "Default", the others "Preset_100000<n>" */
      const key = setKey(k);
      layer = opts.layers ? !!opts.layers[k] : layerOf.has(k);
      presets.push(presetFor(b, key, k));
      if (layer) layersNode.push([key, N(["title", nm], ["legacy_set", "1"], ["set_layer", "1"], ["parent_set_name", setKey(Math.max(0, profiles.findIndex((x, j) => x && !isLayer(j))))])]);
      else actions.push([key, N(["title", nm], ["legacy_set", "1"])]);
      loc.push([key, nm]);
    });
    const title = opts.title || "sc26re remapper";
    const root = N(["controller_mappings", N(
      ["version", "3"], ["revision", "1"], ["title", title], ["description", "exported by the SC2 remapper (sc26re)"], ["creator", ""], ["progenitor", ""], ["url", ""],
      ["export_type", "personal_local"], ["controller_type", "controller_triton"], ["major_revision", "3"], ["minor_revision", "0"], ["Timestamp", "0"],
      ["actions", N(...actions)], ...(layersNode.length ? [["action_layers", N(...layersNode)]] : []), ["localization", N(["english", N(...loc)])],
      ...groups.map(g => ["group", g]), ...presets.map(p => ["preset", p]), ["settings", N()])]);
    return { text: write(root), dropped: [...dropped] };
  }

  const api = { parse, write, get, all, entries, Importer, exportVDF, emptyProfile, parseBinding, KEYS, MODKEYS, PROF_SIZE };
  if (typeof module !== "undefined" && module.exports) module.exports = api; else root.SteamVDF = api;
})(typeof window !== "undefined" ? window : globalThis);
