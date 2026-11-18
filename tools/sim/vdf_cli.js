/* stdin: {text, opts, export?: [profile bytes|null...], exportOpts} -> stdout: {profiles, notes, list, layers, text, dropped} */
const V = require("../../web/vdf.js");
let inp = ""; process.stdin.on("data", d => inp += d).on("end", () => {
  const q = JSON.parse(inp), out = {};
  if (q.text != null) {
    const im = new V.Importer(V.parse(q.text), q.opts || {}); const r = im.run();
    Object.assign(out, { profiles: r.profiles.map(p => p && Array.from(p)), notes: r.notes, list: r.list, layers: r.layers, game: im.gameActions() });
  }
  if (q.export) { const r = V.exportVDF(q.export.map(p => p && Uint8Array.from(p)), q.exportOpts || {}); out.text = r.text; out.dropped = r.dropped; }
  process.stdout.write(JSON.stringify(out));
});
