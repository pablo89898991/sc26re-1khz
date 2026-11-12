#!/usr/bin/env node
// Headless runner for the web patcher engine (web/patcher.html, first <script> block).
// usage: node tools/stock_patch.js <in.fw> <out.fw> [preset|json-opts]
//   controller presets: stock usb1k maxrep balanced fresh   puck presets: stock poll2ms poll1ms poll05 pollmax
"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");

const html = fs.readFileSync(path.join(__dirname, "..", "web", "patcher.html"), "utf8");
const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.error("patch engine script block not found"); process.exit(2); }
const ctx = { module: { exports: {} }, console };
vm.createContext(ctx);
vm.runInContext(m[1] + "\n;globalThis.__e={patch,inspect,locate,puckPatch,puckLocate,puckInspect,fwKind,PUCK_PRESETS};", ctx);
const E = ctx.__e;

// keep in sync with PRESETS in web/patcher.html
const BASE = {fastpath:false,imu:"stock",sflpHz:480,accBatch:null,i2cKhz:400,wtm:null,poll:"stock",usbHz:1000,freerunTicks:33,attr:false,prio:"auto",capA:8,capB:null,defl:"stock",calib:null,name:"Steam Controller"};
const PRESETS = {
  stock:{...BASE}, usb1k:{...BASE,poll:"usbtimer"},
  maxrep:{...BASE,poll:"usbtimer",imu:"g960",sflpHz:480,prio:14},
  balanced:{...BASE,poll:"usbtimer",imu:"g1000",sflpHz:120,accBatch:7,prio:14,wtm:2,fastpath:true},
  fresh:{...BASE,poll:"usbtimer",imu:"g1000",sflpHz:120,accBatch:7,prio:14,wtm:1,fastpath:true},
};

const [inp, outp, sel = ""] = process.argv.slice(2);
if (!inp || !outp) { console.error("usage: stock_patch.js <in.fw> <out.fw> [preset|json]"); process.exit(2); }
const src = new Uint8Array(fs.readFileSync(inp));
const kind = E.fwKind(src);
let r;
if (kind === "puck") {
  const ticks = /^\d+$/.test(sel) ? +sel : E.PUCK_PRESETS[sel || "pollmax"];
  if (ticks === undefined) { console.error("unknown puck preset " + sel); process.exit(2); }
  r = E.puckPatch(src, { pollTicks: ticks });
  if (r.ok) { const v = E.puckLocate(r.out); if (v.waitTicks !== ticks || v.header.crcStored !== v.header.crcActual) r = { ok: false, errors: ["self-check failed"] }; }
} else if (kind === "ibex") {
  const opts = sel.startsWith("{") ? { ...BASE, ...JSON.parse(sel) } : PRESETS[sel || "balanced"];
  if (!opts) { console.error("unknown controller preset " + sel); process.exit(2); }
  r = E.patch(src, opts);
} else { console.error("not an IBEX or PROTEUS firmware file"); process.exit(2); }

if (!r.ok) { (r.errors || []).forEach(e => console.log("ERROR: " + e)); process.exit(1); }
(r.log || []).forEach(([c, msg]) => console.log((c === "ok" ? "ok   " : "warn ") + msg));
fs.writeFileSync(outp, Buffer.from(r.out));
console.log("wrote " + outp);
