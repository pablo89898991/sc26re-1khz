// 3D overlay (web/overlay.html) against the virtual controller: every button, stick, trigger,
// trackpad and the gyro, read natively (sc26re/raw) and from the pad report (what Steam gets),
// with the remapper off and with each of the four profiles active; switching profiles on the
// controller; both models; gyro off, invert, recenter, view mapping, level; the remapper page open
// at the same time (one shared settings channel); two overlays at once; a slow lossy link like the
// Puck. The gyro is checked against the orientation the virtual controller really has.
// Needs a fresh sim_server.py on 8765 and the web folder served on 8766. V=1 prints every check.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const { execFileSync } = require('child_process');
const SRV = 'http://127.0.0.1:8765', WEB = 'http://127.0.0.1:8766';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: typeof b === 'string' ? b : JSON.stringify(b) }).then(r => r.json());
const getJ = p => fetch(SRV + p).then(r => r.json());
let bad = 0, total = 0;
const check = (name, ok, info = '') => { total++; if (!ok) bad++; if (!ok || process.env.V) console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); return ok; };
const T0 = Date.now(), lap = s => process.env.V && console.log(`  [${((Date.now() - T0) / 1000).toFixed(1)} s] ${s}`);
/* ONLY=configs,switching,settings,outage,legacy,replug,models,shared,slow runs some parts */
const want = part => !process.env.ONLY || process.env.ONLY.split(',').includes(part);

async function feature(op, path, data = []) {
  const p = Buffer.from(path), req = Buffer.alloc(64); req[0] = op; req[1] = p.length + 1 + data.length; p.copy(req, 2); Buffer.from(data).copy(req, 3 + p.length);
  return post('/feature', req.toString('hex'));
}
async function cfg(name, v) { await feature(0xEE, 'sc26re/' + name, [v & 255, (v >> 8) & 255]); await feature(0xEF, 'sc26re/' + name); }
async function cfgGet(name) { const r = Buffer.from((await feature(0xED, 'sc26re/' + name)).rsp, 'hex'); return r[1] >= 3 ? r.readInt16LE(3) : null; }
const set = o => post('/set', o);
const REST = { buttons: 0, tl: 0, tr: 0, lsx: 0, lsy: 0, rsx: 0, rsy: 0, lpx: 0, lpy: 0, lpp: 0, rpx: 0, rpy: 0, rpp: 0, gx: 0, gy: 0, gz: 0, ax: 0, ay: 0, az: 16393 };

/* ---- quaternions (w, x, y, z) ---- */
const qmul = (a, b) => [a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3], a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2], a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1], a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]];
const conj = q => [q[0], -q[1], -q[2], -q[3]];
const qang = (a, b) => 2 * Math.acos(Math.min(1, Math.abs(a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3]))) * 180 / Math.PI;
const qaxis = (d) => { const a = Math.hypot(...d) * Math.PI / 180; if (!a) return [1, 0, 0, 0]; const s = Math.sin(a / 2) / Math.hypot(...d); return [Math.cos(a / 2), d[0]*s, d[1]*s, d[2]*s]; };
const qrot = (q, v) => { const p = qmul(qmul(q, [0, ...v]), conj(q)); return [p[1], p[2], p[3]]; };

/* ---- the overlay's picture of the controller against the virtual controller ---- */
const K = 32767, near = (a, b) => Math.abs(a - b) < 1e-4;
const sameState = (st, s) => st.btn === s.buttons >>> 0 && near(st.lt, s.tl / K) && near(st.rt, s.tr / K) && near(st.ls[0], s.lsx / K) && near(st.ls[1], s.lsy / K) &&
  near(st.rs[0], s.rsx / K) && near(st.rs[1], s.rsy / K) && near(st.lp[0], s.lpx / K) && near(st.lp[1], s.lpy / K) && near(st.lp[2], s.lpp / K) &&
  near(st.rp[0], s.rpx / K) && near(st.rp[1], s.rpy / K) && near(st.rp[2], s.rpp / K);
/* raw: the physical controller ("in"), steam: the pad report ("out") */
async function agree(pg, which, ms = 3000) {
  const t0 = Date.now();
  for (;;) {
    const [st, s] = await Promise.all([pg.evaluate(() => JSON.parse(JSON.stringify(__ov.st))), getJ('/state')]);
    if (sameState(st, s[which])) return [true, st, s[which]];
    if (Date.now() - t0 > ms) return [false, st, s[which]];
    await sleep(15);
  }
}
const fmt = (st, s) => `overlay btn ${st.btn.toString(16)} ls ${st.ls.map(v => v.toFixed(3))} lt ${st.lt.toFixed(3)} lp ${st.lp.map(v => v.toFixed(3))} · controller btn ${(s.buttons >>> 0).toString(16)} ls ${s.lsx},${s.lsy} lt ${s.tl} lp ${s.lpx},${s.lpy},${s.lpp}`;

/* what the model shows */
async function visuals(pg) {
  await sleep(60); /* poseParts runs every frame */
  return pg.evaluate(() => { const o = {}; for (const [n, p] of __ov.parts) o[n] = { d: p.pivot.position.distanceTo(p.rest), rx: p.pivot.rotation.x, rz: p.pivot.rotation.z, e: p.material.emissiveIntensity, op: p.material.opacity, tilt: p.tilt };
    for (const s of ['l', 'r']) { const f = __ov.fingers[s]; o['finger_' + s] = f ? { v: f.visible, x: f.position.x, y: f.position.y, k: f.scale.x } : null; } return o; });
}
const PRESS = { 0: 'a', 1: 'b', 2: 'x', 3: 'y', 14: 'view', 6: 'menu', 16: 'steam', 4: 'qam', 19: 'bumper_l', 9: 'bumper_r', 17: 'back_l4', 18: 'back_l5', 7: 'back_r4', 8: 'back_r5' };
const DPAD = { 13: 'up', 10: 'down', 12: 'left', 11: 'right' };
/* every part as it should look with only button bit b held (b = -1: nothing) */
function wrongParts(b, V, valve) {
  const e = [], want = (ok, m) => { if (!ok) e.push(m); };
  const glowNear = (p, k) => Math.abs(p.e - k) < 0.01;
  for (const [bit, n] of Object.entries(PRESS)) { const p = V[n]; if (!p) { e.push('no ' + n); continue; }
    const on = +bit === b, gripGlow = (b === 29 && /back_l/.test(n)) || (b === 28 && /back_r/.test(n));
    want(on ? p.d > 1e-4 && glowNear(p, 0.8) : p.d < 1e-6 && glowNear(p, gripGlow ? 0.25 : 0), `${n} d ${p.d.toFixed(4)} glow ${p.e.toFixed(2)}`); }
  const T = 0.12, rx = b === 13 ? -T : b === 10 ? T : 0, rz = b === 12 ? T : b === 11 ? -T : 0;
  for (const [bit, a] of Object.entries(DPAD)) { const p = V['dpad_' + a]; if (!p) { e.push('no dpad_' + a); continue; }
    want(Math.abs(p.rx - rx) < 1e-6 && Math.abs(p.rz - rz) < 1e-6 && glowNear(p, +bit === b ? 0.8 : 0), `dpad_${a} tilt ${p.rx.toFixed(3)},${p.rz.toFixed(3)} glow ${p.e.toFixed(2)}`); }
  for (const [s, click, touch] of [['l', 15, 24], ['r', 5, 20]]) { const p = V['stick_' + s]; if (!p) { e.push('no stick_' + s); continue; }
    want((b === click ? p.d > 1e-4 : p.d < 1e-6) && glowNear(p, b === click ? 0.8 : b === touch ? 0.45 : 0) && Math.abs(p.rx) < 1e-6 && Math.abs(p.rz) < 1e-6, `stick_${s} d ${p.d.toFixed(4)} glow ${p.e.toFixed(2)}`); }
  for (const [s, touch, click] of [['l', 25, 26], ['r', 21, 22]]) { const p = V['pad_' + s], f = V['finger_' + s]; if (!p || !f) { e.push('no pad/finger ' + s); continue; }
    want((b === click ? p.d > 1e-4 : p.d < 1e-6) && glowNear(p, b === click ? 0.6 : b === touch ? 0.35 : 0) && f.v === (b === touch), `pad_${s} d ${p.d.toFixed(4)} glow ${p.e.toFixed(2)} finger ${f.v}`); }
  for (const [s, click] of [['l', 27], ['r', 23]]) { const p = V['trigger_' + s]; if (!p) { e.push('no trigger_' + s); continue; }
    want(Math.abs(p.rx) < 1e-6 && glowNear(p, b === click ? 0.3 : 0), `trigger_${s} swing ${p.rx.toFixed(3)} glow ${p.e.toFixed(2)}`); }
  if (valve) for (const [s, g] of [['l', 29], ['r', 28]]) { const p = V['grip_' + s]; if (!p) { e.push('no grip_' + s); continue; } want(Math.abs(p.op - (b === g ? 0.85 : 0)) < 1e-6, `grip_${s} ${p.op}`); }
  return e;
}

/* ---- gyro: the overlay's turn against the virtual controller's own orientation ---- */
async function mark(pg) { const [ov, t] = await Promise.all([pg.evaluate(() => __ov.q.slice()), getJ('/truth')]); return { ov, t: t.q, deg: t.deg }; }
async function turnErr(pg, m) { const now = await mark(pg); return [qang(qmul(conj(m.ov), now.ov), qmul(conj(m.t), now.t)), qang(m.t, now.t), now]; }
async function turn(segs) { for (const [r, ms] of segs) { await set({ gx: r[0], gy: r[1], gz: r[2] }); await sleep(ms); } await set({ gx: 0, gy: 0, gz: 0 }); }
/* until the overlay has seen the controller at rest: the pad reports up to now, and (native) two reads after that */
async function settle(pg) {
  const ts = Buffer.from(await (await fetch(SRV + '/report42')).text(), 'hex').readUInt32LE(29);
  await pg.waitForFunction(ts => __ov.lastTs !== null && ((__ov.lastTs - ts) | 0) >= 0, ts, { timeout: 30000 }).catch(() => {});
  if (await pg.evaluate(() => __ov.src) === 'raw') { const c = await pg.evaluate(() => __ov.natCount); await pg.waitForFunction(c => __ov.natCount >= c + 2, c, { timeout: 30000 }).catch(() => {}); }
  await sleep(30);
}
const MOVES = [[[900, 0, 0], 250], [[0, -700, 500], 300], [[-400, 800, -900], 250], [[1200, 300, 0], 200]];
async function gyroCheck(pgs, name, tol, segs = MOVES, minTurn = 10) {
  if (!Array.isArray(pgs)) pgs = [pgs];
  for (const pg of pgs) await settle(pg);
  const ms = []; for (const pg of pgs) ms.push(await mark(pg));
  await turn(segs);
  for (const pg of pgs) await settle(pg);
  for (let i = 0; i < pgs.length; i++) { const [e, tt] = await turnErr(pgs[i], ms[i]);
    check(name + (pgs.length > 1 ? ' (overlay ' + (i + 1) + ')' : ''), e < tol && tt > minTurn, `off by ${e.toFixed(3)}° after turning ${tt.toFixed(1)}°`); }
}
async function qSane(pg, name) { const q = await pg.evaluate(() => __ov.q.slice()); check(name + ': orientation stays a unit quaternion', q.every(Number.isFinite) && Math.abs(Math.hypot(...q) - 1) < 1e-6, JSON.stringify(q)); }

async function setCtl(pg, id, v) { await pg.evaluate(([id, v]) => { const e = document.getElementById(id); if (e.type === 'checkbox') e.checked = v; else e.value = v; e.dispatchEvent(new Event('input')); e.dispatchEvent(new Event('change')); }, [id, v]); }
async function openOverlay(br, shim, name) {
  const pg = await br.newPage({ viewport: { width: 480, height: 360 } });
  pg.__errs = []; pg.on('pageerror', e => pg.__errs.push(name + ': ' + e.message));
  pg.on('console', m => { if (m.type() === 'error' && !/favicon\.ico/.test(m.location().url || '')) pg.__errs.push(name + ': ' + m.text() + ' @' + (m.location().url || '')); });
  pg.on('response', r => { if (r.status() >= 400 && !/favicon\.ico/.test(r.url())) pg.__errs.push(name + ': HTTP ' + r.status() + ' ' + r.url()); });
  await pg.addInitScript({ path: __dirname + '/' + shim });
  await pg.addInitScript(() => { try { localStorage.removeItem('sc26re_overlay2'); } catch (e) {} });
  await pg.goto(WEB + '/overlay.html#debug');
  const ok = await pg.waitForFunction(() => window.__ov && __ov.ready, null, { timeout: 60000 }).then(() => true, () => false);
  check(name + ': model loaded', ok);
  await pg.click('#connect');
  check(name + ': native reads', await pg.waitForFunction(() => __ov.raw[0] && __ov.raw[2] - __ov.raw[1] < 300, null, { timeout: 15000 }).then(() => true, () => false));
  await sleep(400); /* the page recenters 0.3 s after connecting */
  return pg;
}

/* all 30 buttons one at a time, the sticks, triggers and trackpads */
const ANALOG = [{ lsx: 24000, lsy: -12000 }, { rsx: -30000, rsy: 20000 }, { tl: 20000 }, { tr: 32767 }, { buttons: 1 << 25, lpx: 10000, lpy: -8000, lpp: 16000 },
  { buttons: (1 << 21) | (1 << 22), rpx: -20000, rpy: 15000, rpp: 32767 }, { lsx: -32768, lsy: 32767, buttons: 1 << 15 }];
async function allInputs(pg, which, label, opts = {}) {
  const valve = (await pg.evaluate(() => document.getElementById('mdl').value)) === 'valve';
  let wrongBtn = [], wrongVis = [];
  /* every state the overlay shows, in order */
  await pg.evaluate(() => { window.__seen = [__ov.st.btn]; clearInterval(window.__seenT); window.__seenT = setInterval(() => { const b = __ov.st.btn; if (__seen[__seen.length - 1] !== b) __seen.push(b); }, 1); });
  for (let b = -1; b < 30; b++) {
    await set({ ...REST, buttons: b < 0 ? 0 : (1 << b) >>> 0 });
    const [ok, st, s] = await agree(pg, which); if (!ok) wrongBtn.push(`bit ${b}: ${fmt(st, s)}`);
    if (opts.visuals && ok) { const w = wrongParts(which === 'in' ? b : -2, await visuals(pg), valve); if (which === 'in' && w.length) wrongVis.push(`bit ${b}: ${w.join(', ')}`); }
  }
  check(`${label}: each button shows`, !wrongBtn.length, wrongBtn.slice(0, 3).join(' | '));
  /* the physical controller: one button after the other, a stale reply would bring an earlier one back */
  if (which === 'in') { const seen = await pg.evaluate(() => { clearInterval(window.__seenT); return __seen; }), order = [0, ...Array.from({ length: 30 }, (_, i) => (1 << i) >>> 0)];
    const idx = seen.map(v => order.indexOf(v >>> 0)), back = idx.findIndex((v, i) => i && v >= 0 && idx[i - 1] >= 0 && v < idx[i - 1]);
    check(`${label}: never shows an older state again`, back < 0, back < 0 ? '' : `showed ${seen[back - 1].toString(16)} then ${seen[back].toString(16)}`); }
  else await pg.evaluate(() => clearInterval(window.__seenT));
  if (opts.visuals) check(`${label}: each button moves / lights its part`, !wrongVis.length, wrongVis.slice(0, 3).join(' | '));
  let wrongA = [];
  for (const a of ANALOG) {
    await set({ ...REST, ...a });
    const [ok, st, s] = await agree(pg, which); if (!ok) { wrongA.push(fmt(st, s)); continue; }
    if (!opts.visuals || which !== 'in') continue;
    const V = await visuals(pg), tilt = valve ? 0.436 : 0.3, k = v => v / K;
    if (a.lsx !== undefined && !(Math.abs(V.stick_l.rz + k(a.lsx) * tilt) < 1e-4 && Math.abs(V.stick_l.rx + k(a.lsy) * tilt) < 1e-4)) wrongA.push(`left stick tilt ${V.stick_l.rx.toFixed(3)},${V.stick_l.rz.toFixed(3)}`);
    if (a.rsx !== undefined && !(Math.abs(V.stick_r.rz + k(a.rsx) * tilt) < 1e-4 && Math.abs(V.stick_r.rx + k(a.rsy) * tilt) < 1e-4)) wrongA.push(`right stick tilt ${V.stick_r.rx.toFixed(3)},${V.stick_r.rz.toFixed(3)}`);
    for (const [f, s] of [['tl', 'l'], ['tr', 'r']]) if (a[f] !== undefined && !(Math.abs(V['trigger_' + s].rx + k(a[f]) * 0.35) < 1e-4 && Math.abs(V['trigger_' + s].e - (0.3 + k(a[f]) * 0.5)) < 1e-3)) wrongA.push(`trigger ${s} swing ${V['trigger_' + s].rx.toFixed(3)}`);
    for (const [s, x, y, p] of [['l', 'lpx', 'lpy', 'lpp'], ['r', 'rpx', 'rpy', 'rpp']]) if (a[x] !== undefined) { const f = V['finger_' + s];
      if (!(f.v && Math.sign(f.x) === Math.sign(a[x]) && Math.sign(f.y) === Math.sign(a[y]) && Math.abs(f.x / f.y - a[x] / a[y]) < 1e-3 && Math.abs(f.k - (1 + k(a[p]) * 2.7)) < 1e-3)) wrongA.push(`finger ${s} ${JSON.stringify(f)}`); }
  }
  check(`${label}: sticks, triggers and trackpads show`, !wrongA.length, wrongA.slice(0, 3).join(' | '));
  await set(REST);
}

(async () => {
  console.log(execFileSync('python3', [__dirname + '/overlay_profiles.py']).toString().trim());
  await set(REST);
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  const pages = [];
  const pg = await openOverlay(br, 'webhid_shim.js', 'overlay'); pages.push(pg);
  check('status shows the native read rate', await pg.waitForFunction(() => /native (\d+)\/s/.test(document.getElementById('status').textContent) && +document.getElementById('status').textContent.match(/native (\d+)\/s/)[1] > 5, null, { timeout: 5000 }).then(() => true, () => false),
    await pg.textContent('#status'));
  lap('connected');

  /* ---- the remapper off, then each profile active ---- */
  const CONFIGS = [['remapper off', 0, -1, 1], ['profile 1 (keys)', 1, 0, 1], ['profile 2 (Xbox)', 1, 1, 1], ['profile 3 (modes)', 1, 2, 1], ['profile 4 (shift, gyro mouse)', 1, 3, 1], ['profile 2, no Xbox pad at the PC', 1, 1, 0]];
  for (const [label, on, slot, xbox] of want('configs') ? CONFIGS : []) {
    await post('/xbox', { on: xbox });
    if (slot >= 0) await cfg('remap_profile', slot);
    await cfg('remap_on', on);
    check(`${label}: active`, (await cfgGet('remap_on')) === on && (slot < 0 || (await cfgGet('remap_profile')) === slot));
    const start = await mark(pg);
    await allInputs(pg, 'in', label + ', native', { visuals: label === 'remapper off' || slot === 3 });
    await gyroCheck(pg, label + ', native: gyro turns it like the controller', 0.05);
    /* what Steam receives */
    await setCtl(pg, 'src', 'steam'); await sleep(100);
    await allInputs(pg, 'out', label + ', what Steam receives');
    /* a few things that must differ from the physical controller */
    const steamBtn = async (b) => { await set({ ...REST, buttons: 1 << b }); const [ok, st] = await agree(pg, 'out'); await set(REST); await agree(pg, 'out'); return ok ? st.btn : -1; };
    const sb = async (name, b, want) => { const v = await steamBtn(b); check(`${label}, Steam view: ${name}`, v === want, 'shows ' + v.toString(16)); };
    if (slot === 0) { await sb('A (a key) is not sent to Steam', 0, 0); await sb('B (unbound, hidden) is not sent', 1, 0); await sb('X (pad X) is', 2, 4); }
    if (slot === 1) await sb(`A (Xbox B) ${xbox ? 'goes to the Xbox pad' : 'reaches Steam as B'}`, 0, xbox ? 0 : 2);
    if (slot < 0) await sb('A is A', 0, 1);
    if (slot === 3) { /* the gyro mouse is on with this profile: the pad report carries no motion */
      await settle(pg); const m = await mark(pg); await turn(MOVES); await settle(pg);
      const [, tt, now] = await turnErr(pg, m);
      check(`${label}, what Steam receives: no motion while the gyro mouse is on`, qang(m.ov, now.ov) < 0.01 && tt > 10, `moved ${qang(m.ov, now.ov).toFixed(3)}° while the controller turned ${tt.toFixed(1)}°`);
    } else await gyroCheck(pg, label + ', what Steam receives: gyro turns it like the controller', 0.05);
    await settle(pg); const before = await mark(pg);
    await setCtl(pg, 'src', 'raw'); await sleep(300); await settle(pg);
    const [jump] = await turnErr(pg, before);
    check(`${label}: back to native, no jump`, jump < 0.05, `jumped ${jump.toFixed(2)}°`);
    if (slot !== 3) { const [e, tt] = await turnErr(pg, start); check(`${label}: all the turning added up`, e < 0.05, `off by ${e.toFixed(3)}° after ${tt.toFixed(1)}°`); }
    await qSane(pg, label);
    lap(label);
  }

  /* ---- profiles switched on the controller (Steam + D-pad) with the overlay open ---- */
  await cfg('remap_on', 1); await post('/xbox', { on: 1 });
  for (const [dp, slot] of want('switching') ? [[11, 1], [10, 2], [12, 3], [13, 0]] : []) {
    await set({ ...REST, buttons: 1 << 16 }); await agree(pg, 'in');
    await set({ ...REST, buttons: (1 << 16) | (1 << dp) });
    const [ok] = await agree(pg, 'in');
    await set({ ...REST, buttons: 1 << 16 }); await agree(pg, 'in'); await set(REST); await agree(pg, 'in');
    check(`Steam + D-pad ${DPAD[dp]}: the overlay shows both, profile ${slot + 1} active`, ok && (await cfgGet('remap_profile')) === slot);
  }
  if (want('switching')) await gyroCheck(pg, 'after switching profiles: gyro still right', 0.05);
  lap('profile switching');

  /* ---- settings ---- */
  if (want('settings')) {
    await settle(pg); await setCtl(pg, 'useGyro', false); const m = await mark(pg); await turn(MOVES); await settle(pg);
    const [, tt, now] = await turnErr(pg, m); check('gyro off: the model stays still', qang(m.ov, now.ov) < 1e-4 && tt > 10, `moved ${qang(m.ov, now.ov).toFixed(3)}° while the controller turned ${tt.toFixed(1)}°`);
    await setCtl(pg, 'useGyro', true); const m2 = await mark(pg); await sleep(300); await settle(pg);
    const [jump] = await turnErr(pg, m2); check('gyro back on: no jump', jump < 0.05, jump.toFixed(3));
    for (const [id, axis] of [['invP', 0], ['invR', 1], ['invY', 2]]) {
      await setCtl(pg, id, true); await settle(pg);
      const m = await mark(pg); const r = [700, -500, 600]; await turn([[r, 300]]); await settle(pg);
      const now = await mark(pg); const d = now.deg.map((v, i) => (v - m.deg[i]) * (i === axis ? -1 : 1));
      check(`invert ${id.slice(3)}: turns the other way about that axis only`, qang(qmul(conj(m.ov), now.ov), qaxis(d)) < 0.05, qang(qmul(conj(m.ov), now.ov), qaxis(d)).toFixed(3));
      await setCtl(pg, id, false);
    }
    /* recenter: the model faces the camera again (tilt 0: face to the camera, triggers up) */
    await setCtl(pg, 'tilt', 0); await turn(MOVES); await sleep(300); await pg.keyboard.press('r'); await sleep(100);
    const front = await pg.evaluate(() => { const q = __ov.model.quaternion; return [q.w, q.x, q.y, q.z]; });
    check('R recenters: face to the camera, triggers up', qang(front, [Math.cos(Math.PI / 4), Math.sin(Math.PI / 4), 0, 0]) < 0.01, JSON.stringify(front));
    /* the sensor frame on screen: +x right, +y towards the triggers, +z out of the face */
    for (const [name, r, part, want] of [['pitch up: the triggers come towards the camera', [1475, 0, 0], [0, 0, -1], [0, 0, 1]], ['yaw left: the triggers turn left', [0, 0, 1475], [0, 0, -1], [-1, 0, 0]],
      ['roll: the right side goes away from the camera', [0, 1475, 0], [1, 0, 0], [0, 0, -1]]]) {
      await pg.keyboard.press('r'); await sleep(100); await turn([[r, 1000]]); await sleep(300);
      const dir = await pg.evaluate(p => { const v = new __ov.model.position.constructor(...p).applyQuaternion(__ov.model.quaternion); return [v.x, v.y, v.z]; }, part);
      check(name, dir[0] * want[0] + dir[1] * want[1] + dir[2] * want[2] > 0.95, dir.map(v => v.toFixed(2)).join(','));
    }
    await pg.keyboard.press('r'); await setCtl(pg, 'tilt', 20);
    /* level: the gyro says it tipped 40 degrees, gravity says it lies flat: it settles flat */
    await turn([[[1300, 0, 0], 500]]); await sleep(300);
    const tipped = await pg.evaluate(() => __ov.q.slice()); await setCtl(pg, 'level', true); const t0 = Date.now();
    const up = q => Math.acos(Math.max(-1, Math.min(1, qrot(q, [0, 0, 1])[2]))) * 180 / Math.PI;
    let lv; do { await sleep(200); lv = await pg.evaluate(() => __ov.q.slice()); } while (up(lv) >= 1.5 && Date.now() - t0 < 8000);
    check('keep level: settles flat with gravity', up(tipped) > 20 && up(lv) < 1.5, `tipped ${up(tipped).toFixed(1)}°, ${up(lv).toFixed(2)}° after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
    await setCtl(pg, 'level', false); await pg.keyboard.press('r');
    /* the mouse turns the view, a double-click puts it back; H hides the panel */
    { const mq = () => pg.evaluate(() => { const q = __ov.model.quaternion; return [q.w, q.x, q.y, q.z]; });
      await settle(pg); await sleep(100); const before = await mq();
      await pg.mouse.move(465, 345); await pg.mouse.down(); await pg.mouse.move(420, 310, { steps: 5 }); await pg.mouse.up(); await sleep(100); /* off the panel */
      const dragged = await mq(); await pg.mouse.dblclick(465, 345); await sleep(100); const back = await mq();
      check('dragging with the mouse turns the view', qang(before, dragged) > 10, qang(before, dragged).toFixed(1) + '°');
      check('a double-click puts the view back', qang(before, back) < 0.01, qang(before, back).toFixed(3) + '°');
      await pg.keyboard.press('h'); const hid = await pg.evaluate(() => document.getElementById('ui').classList.contains('hidden'));
      await pg.keyboard.press('h'); const shown = await pg.evaluate(() => !document.getElementById('ui').classList.contains('hidden'));
      check('H hides and shows the panel', hid && shown); }
    await qSane(pg, 'settings');
    lap('settings');
  }

  /* ---- native reads stop for a while (the controller busy, the channel taken) while it turns ---- */
  for (const ms of want('outage') ? [1500, 5000] : []) {
    await settle(pg); const m = await mark(pg);
    await post('/block', { path: 'sc26re/raw', ms }); const t0 = Date.now();
    await turn([[[500, -300, 200], 600], [[0, 800, -400], ms - 1100]]);
    /* still no native reads: the pad reports keep it turning meanwhile (to within their timing) */
    const ts = Buffer.from(await (await fetch(SRV + '/report42')).text(), 'hex').readUInt32LE(29);
    await pg.waitForFunction(ts => __ov.lastTs !== null && ((__ov.lastTs - ts) | 0) >= 0, ts, { timeout: 5000 }).catch(() => {});
    const [em, ttm] = await turnErr(pg, m), left = ms - (Date.now() - t0);
    check(`native reads stop for ${ms / 1000} s while it turns: the pad reports keep it turning`, left > 50 && em < 5 && ttm > 10, `off by ${em.toFixed(2)}° after turning ${ttm.toFixed(1)}° (${left} ms of the stop left)`);
    await sleep(Math.max(0, left) + 100); await settle(pg);
    const [e, tt] = await turnErr(pg, m);
    check(`native reads stop for ${ms / 1000} s while it turns: right again when they come back`, e < 0.05 && tt > 10, `off by ${e.toFixed(3)}° after turning ${tt.toFixed(1)}°`);
  }
  lap('native outage');

  /* ---- firmware up to v51 test 42 (the turn since the last read, no reply time): still works ---- */
  if (want('legacy')) {
    await post('/legacy', { on: 1 }); await sleep(300);
    await allInputs(pg, 'in', 'v51 test 42 firmware, native');
    for (const [r, name] of [[[900, 0, 0], 'pitch'], [[0, -700, 0], 'roll'], [[0, 0, 1100], 'yaw'], [[500, -400, 300], 'all three at once']])
      await gyroCheck(pg, `v51 test 42 firmware: ${name} turns it like the controller`, 0.05, [[r, 600]], 10);
    await post('/legacy', { on: 0 }); await sleep(300); await settle(pg);
    await gyroCheck(pg, 'back to the new firmware: gyro right', 0.05);
    lap('legacy');
  }

  /* ---- unplugged and plugged in again; the controller restarting behind the Puck ---- */
  if (want('replug')) {
    await settle(pg); const q0 = await pg.evaluate(() => __ov.q.slice());
    await pg.evaluate(() => __unplug()); await sleep(300);
    check('unplugged: the overlay says so', /disconnected/.test(await pg.textContent('#status')), await pg.textContent('#status'));
    await turn([[[600, 200, -300], 800]]); /* turned while unplugged: the overlay cannot know */
    await pg.evaluate(() => __plug()); await sleep(500); await settle(pg);
    const q1 = await pg.evaluate(() => __ov.q.slice());
    check('plugged in again: carries on from where it stood (no jump)', qang(q0, q1) < 0.05, `moved ${qang(q0, q1).toFixed(3)}°`);
    await gyroCheck(pg, 'plugged in again: turns like the controller', 0.05);
    await setCtl(pg, 'src', 'steam'); await gyroCheck(pg, 'plugged in again, what Steam receives: each pad report counts once', 0.05); await setCtl(pg, 'src', 'raw'); await sleep(300);
    await settle(pg); const q2 = await pg.evaluate(() => __ov.q.slice());
    await post('/restart', {}); await sleep(600); await settle(pg);
    const q3 = await pg.evaluate(() => __ov.q.slice());
    check('the controller restarts (its orientation starts over): no jump', qang(q2, q3) < 0.05, `moved ${qang(q2, q3).toFixed(3)}°`);
    await gyroCheck(pg, 'after the restart: turns like the controller', 0.05);
    lap('replug');
  }

  /* ---- the detailed model ---- */
  if (want('models')) {
  await setCtl(pg, 'mdl', 'detail');
  check('detailed model loads', await pg.waitForFunction(() => __ov.ready && __ov.parts.has('part_a') === false && __ov.parts.has('a') && !__ov.parts.has('grip_l'), null, { timeout: 60000 }).then(() => true, () => false));
  await cfg('remap_on', 0);
  await allInputs(pg, 'in', 'detailed model, native', { visuals: true });
  await gyroCheck(pg, 'detailed model: gyro', 0.05);
  await setCtl(pg, 'mdl', 'valve');
  check('back to Valve\'s model', await pg.waitForFunction(() => __ov.ready && __ov.parts.has('grip_l') && __ov.fingers.l && __ov.fingers.r, null, { timeout: 60000 }).then(() => true, () => false));
  lap('models');
  }

  /* ---- the remapper page open at the same time: one settings channel for both ---- */
  if (want('shared')) {
  await cfg('remap_profile', 0); await cfg('remap_on', 1);
  const rp = await br.newPage({ viewport: { width: 1300, height: 1000 } }); pages.push(rp);
  rp.__errs = []; rp.on('pageerror', e => rp.__errs.push('remap: ' + e.message)); rp.on('dialog', d => d.accept());
  await rp.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await rp.goto('file://' + __dirname + '/../../web/remap.html');
  await rp.click('#connect');
  check('remapper page: loads the profiles while the overlay reads', await rp.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 }).then(() => true, () => false),
    await rp.textContent('#pstatus'));
  check('both pages are visible (the remapper page polls the controller)', !(await rp.evaluate(() => document.hidden)) && !(await pg.evaluate(() => document.hidden)) && await rp.evaluate(() => remapOn));
  await setCtl(pg, 'imuOn', true); /* the overlay also keeps the gyro on (another command on the channel) */
  await sleep(500);
  await gyroCheck(pg, 'remapper page open: the overlay still turns exactly like the controller', 0.05);
  await gyroCheck(pg, 'remapper page open: slow turns too', 0.05, [[[60, 40, -50], 1500], [[-30, 0, 90], 1500]], 3);
  await set({ ...REST, buttons: 2 }); await agree(pg, 'in');
  check('remapper page open: its picture shows the physical B', await rp.waitForFunction(() => (lastBtn & 2) === 2, null, { timeout: 3000 }).then(() => true, () => false));
  await set(REST);
  { let ok = 0; for (let i = 0; i < 10; i++) { try { const v = await rp.evaluate(() => cfgGet('remap_profile')); if (v === 0) ok++; } catch (e) {} } check('remapper page open: its own reads work', ok === 10, ok + '/10'); }
  {
    await rp.evaluate(() => { prof[2][2] = 0x4F; prof[2][3] = 0x56; setDirty(true, 2); });
    await rp.click('#write');
    await rp.waitForFunction(() => /saved|differs|err/.test(document.querySelector('#pstatus').innerHTML), null, { timeout: 90000 }).catch(() => {});
    check('remapper page open: saving a profile works while the overlay reads', /saved/.test(await rp.textContent('#pstatus')), await rp.textContent('#pstatus'));
    const back = await rp.evaluate(async () => Array.from(await readProfile(2)).slice(2, 4));
    check('remapper page open: the saved profile reads back', back[0] === 0x4F && back[1] === 0x56, back.join(','));
  }
  /* (the rate depends on the PC: here two cores draw three pages without a GPU) */
  check('remapper page open: the overlay keeps reading natively', await pg.waitForFunction(() => /native [1-9]\d*\/s/.test(document.getElementById('status').textContent), null, { timeout: 5000 }).then(() => true, () => false), await pg.textContent('#status'));
  /* two overlays (a browser window and OBS, say) */
  const pg2 = await openOverlay(br, 'webhid_shim.js', 'second overlay'); pages.push(pg2);
  await gyroCheck([pg, pg2], 'two overlays and the remapper page: both turn like the controller', 0.05);
  await allInputs(pg2, 'in', 'second overlay, native');
  lap('shared channel');
  await setCtl(pg, 'imuOn', false);
  for (const p of [pg2, rp]) await p.close();
  }
  await pg.close();

  /* ---- a slow, lossy link like the Puck: late, lost and stale replies ---- */
  if (want('slow')) {
  await cfg('remap_profile', 2); await cfg('remap_on', 1);
  const sp = await openOverlay(br, 'webhid_shim_slow.js', 'slow link'); pages.push(sp);
  await allInputs(sp, 'in', 'slow link, native', { visuals: true });
  await gyroCheck(sp, 'slow link: gyro turns it like the controller', 0.05);
  await gyroCheck(sp, 'slow link: slow turns too', 0.05, [[[60, 40, -50], 1500], [[-30, 0, 90], 1500]], 3);
  const ls = await sp.evaluate(() => __linkStats);
  check('slow link: lost and stale replies happened', ls.lostReply > 0 && ls.stale > 0, JSON.stringify(ls));
  await qSane(sp, 'slow link');
  lap('slow link');
  }

  const errs = pages.flatMap(p => p.__errs || []);
  check('no page errors', !errs.length, errs.slice(0, 4).join(' | '));
  console.log(`${total} checks, ${bad} failed`);
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e.stack || e.message); process.exit(1); });
