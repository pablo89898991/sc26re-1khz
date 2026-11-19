/* random profiles -> export -> import; mutated .vdf text -> import: nothing may throw, profiles stay 3232 bytes v8 */
const V = require("../../web/vdf.js"), fs = require("fs");
let seed = +(process.argv[2] || 1); const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
const ri = n => Math.floor(rnd() * n);
let n = 0, fails = 0;
/* what Steam needs of an exported layout: unique group ids, presets naming existing groups,
 * mode shifts pointing at a modeshift group of the same source in the same preset, every
 * input with at least one activator holding at least one binding, layer / set switches in range */
function schema(tree) {
  const root = V.get(tree, "controller_mappings"); if (!root) throw new Error("no controller_mappings");
  const groups = V.all(root, "group"), ids = groups.map(g => V.get(g, "id"));
  if (new Set(ids).size !== ids.length) throw new Error("duplicate group ids");
  const presets = V.all(root, "preset"), pids = presets.map(p => +V.get(p, "id"));
  const names = new Set([...V.entries(V.get(root, "actions")).map(([k]) => k), ...V.entries(V.get(root, "action_layers")).map(([k]) => k)]);
  for (const p of presets) {
    if (!names.has(V.get(p, "name"))) throw new Error("preset without an action set " + V.get(p, "name"));
    const gsb = V.entries(V.get(p, "group_source_bindings"));
    const act = {};
    for (const [gid, w] of gsb) { if (!ids.includes(gid)) throw new Error("preset names a missing group " + gid); const [src, ...f] = w.split(" "); if (f.includes("active") && !f.includes("modeshift")) { if (act[src]) throw new Error("two active groups for " + src); act[src] = gid; } }
    for (const [gid] of gsb) {
      const g = groups[ids.indexOf(gid)];
      for (const [name, inp] of V.entries(V.get(g, "inputs"))) {
        const acts = V.entries(V.get(inp, "activators"));
        if (!acts.length) throw new Error("input " + name + " without activators");
        for (const [, a] of acts) {
          const bs = V.all(V.get(a, "bindings"), "binding"); if (!bs.length) throw new Error("activator without bindings");
          for (const bnd of bs) {
            const w = bnd.split(",")[0].trim().split(/\s+/);
            if (w[0] === "mode_shift") { const ok = gsb.some(([id, ww]) => id === w[2] && ww.startsWith(w[1] + " ") && ww.includes("modeshift")); if (!ok) throw new Error("mode_shift to a group not shifted in this preset: " + bnd); }
            if (w[0] === "controller_action" && /^(CHANGE_PRESET|hold_layer)$/i.test(w[1]) && !pids.includes(+w[2] - 1)) throw new Error("switch to a missing set " + bnd);
            if (w[0] === "controller_action" && /^(CHANGE_PRESET|hold_layer)$/i.test(w[1])) {
              const tp = presets[pids.indexOf(+w[2] - 1)], isL = V.entries(V.get(root, "action_layers")).some(([k]) => k === V.get(tp, "name"));
              if (/hold_layer/i.test(w[1]) !== isL) throw new Error((isL ? "action set switch to a layer " : "layer that is an action set ") + bnd);
            }
          }
        }
      }
    }
  }
}
function chk(r) { for (const p of r.profiles) if (p && (p.length !== 3232 || p[0] !== 0xA7 || p[1] !== 8)) throw new Error("bad profile"); }
for (let k = 0; k < 300; k++) {
  const P = [0, 1, 2, 3].map(() => { if (rnd() < 0.15) return null; const b = V.emptyProfile("f" + k); const m = ri(4);
    for (let j = 0; j < 400; j++) b[ri(3232)] = m === 0 ? ri(256) : [0, 1, 2, 3, 4, 10, 11, 12, 13, 0xC4, 0xC5, 0xDA, 0x70, 0x2C, 0xE1][ri(15)]; b[0] = 0xA7; b[1] = 8; return b; });
  try { const e = V.exportVDF(P, {}); schema(V.parse(e.text)); const im = new V.Importer(V.parse(e.text)); chk(im.run()); n++; }
  catch (err) { fails++; if (fails < 5) console.log("export/import", k, err.stack); }
}
const base = fs.readFileSync(process.argv[3], "utf8");
for (let k = 0; k < 400; k++) {
  let t = base; const ops = 1 + ri(20);
  for (let j = 0; j < ops; j++) { const a = ri(t.length), l = ri(200); const c = ri(4);
    t = c === 0 ? t.slice(0, a) + t.slice(a + l) : c === 1 ? t.slice(0, a) + t.slice(ri(t.length), ri(t.length)) + t.slice(a) : c === 2 ? t.slice(0, a) + ["{", "}", '"', "\n", "0", "-1", "99999", "reference"][ri(8)] + t.slice(a) : t.slice(0, a); }
  try { const im = new V.Importer(V.parse(t), { gameMap: { Attack: 0x70 } }); im.gameActions(); chk(im.run()); n++; }
  catch (err) { fails++; if (fails < 8) console.log("mutated", k, err.stack.split("\n").slice(0, 3).join(" | ")); }
}
console.log(n, "ok,", fails, "failed"); process.exit(fails ? 1 : 0);
