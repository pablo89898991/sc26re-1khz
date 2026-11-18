/* random profiles -> export -> import; mutated .vdf text -> import: nothing may throw, profiles stay 3144 bytes v7 */
const V = require("../../web/vdf.js"), fs = require("fs");
let seed = +(process.argv[2] || 1); const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
const ri = n => Math.floor(rnd() * n);
let n = 0, fails = 0;
function chk(r) { for (const p of r.profiles) if (p && (p.length !== 3144 || p[0] !== 0xA7 || p[1] !== 7)) throw new Error("bad profile"); }
for (let k = 0; k < 300; k++) {
  const P = [0, 1, 2, 3].map(() => { if (rnd() < 0.15) return null; const b = V.emptyProfile("f" + k); const m = ri(4);
    for (let j = 0; j < 400; j++) b[ri(3144)] = m === 0 ? ri(256) : [0, 1, 2, 3, 4, 10, 11, 12, 13, 0xC4, 0xC5, 0xDA, 0x70, 0x2C, 0xE1][ri(15)]; b[0] = 0xA7; b[1] = 7; return b; });
  try { const e = V.exportVDF(P, {}); const im = new V.Importer(V.parse(e.text)); chk(im.run()); n++; }
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
