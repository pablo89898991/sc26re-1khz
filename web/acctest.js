// Hardware test of the remapper's accelerometer (firmware v51.8+ built with CONFIG_IBEX_HWTEST): tilt -> stick,
// tilt -> buttons and gestures, driven by the firmware's hardware test (scripted accelerometer samples),
// on profile 4, which is put back afterwards with every setting touched. Needs web/hwreg.js loaded first.
//   ACCTEST.run().then(r => console.log(r))
(() => {
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  async function run() {
    const { rd, snd, get, set } = HWREG;
    if (!await HWREG.connect()) return 'no controller';
    const bRead = async (pre, size) => { const out = new Uint8Array(size); for (let o = 0; o < size;) { const x = await rd(pre + '/' + o.toString(16)); if (!x.length) return null; out.set(x.slice(0, size - o), o); o += x.length; } return out; };
    const bWrite = async (pre, b) => { for (let a = 0; a < 4; a++) { for (let o = 0; o < b.length; o += 40) await snd(pre + '/' + o.toString(16), 0xEE, Array.from(b.slice(o, o + 40))); await snd(pre, 0xEF); const r = await bRead(pre, b.length); if (r && r.every((v, k) => v === b[k])) return true; } return false; };
    const bk = { prof3: await bRead('sc26re/rmp/3', 3232), acc3: await bRead('sc26re/acc/3', 32), rp: await get('remap_profile'), on: await get('remap_on'), gm: await get('gm_enabled'), act: await get('gm_act_mode') };
    const res = []; let err = null, restored = false;
    const runScript = async steps => {
      const sb = new Uint8Array(steps.length * 42), v = new DataView(sb.buffer);
      steps.forEach((s, i) => { const o = i * 42; v.setUint16(o, s.ms, true); v.setInt16(o + 36, s.ax || 0, true); v.setInt16(o + 38, s.ay || 0, true); v.setInt16(o + 40, s.az || 0, true); });
      for (let o = 0; o < sb.length; o += 40) await snd('sc26re/hwt', 0xEE, [1, o & 255, o >> 8, ...sb.slice(o, o + 40)]);
      await snd('sc26re/hwt', 0xEE, [2, steps.length, 0]); await sleep(steps.reduce((a, s) => a + s.ms, 0) + 250);
      let st; for (let k = 0; k < 60; k++) { st = await rd('sc26re/hwt_st'); if (st.length && !(st[0] & 6)) break; await sleep(100); }
      const n = st[2] | st[3] << 8, log = []; await snd('sc26re/hwt', 0xEE, [4, 0, 0]);
      for (let i = 0, g = 0; i < n && g < n * 8; g++) {
        const d = await rd('sc26re/hwt_log');
        if (d.length < 56 || (d[0] | d[1] << 8) !== i) { await snd('sc26re/hwt', 0xEE, [4, i & 255, i >> 8]); continue; }
        const dv = new DataView(d.buffer, d.byteOffset + 2, 54);
        log.push({ t: dv.getUint16(0, true), btn: dv.getUint32(12, true), lsx: dv.getInt16(42, true), lsy: dv.getInt16(44, true), rsx: dv.getInt16(46, true), rsy: dv.getInt16(48, true) }); i++;
      }
      await snd('sc26re/hwt', 0xEE, [3]); return { flags: st[0], log };
    };
    try {
      const G = 8192, R = Math.PI / 180; // 1 g = 8192 here: the firmware measures in units of its own gravity average
      const tilt = (f, s) => { f *= R; s *= R; return { ax: Math.round(-Math.sin(s) * G), ay: Math.round(-Math.sin(f) * Math.cos(s) * G), az: Math.round(Math.cos(f) * Math.cos(s) * G) }; };
      // profile: Tilt forward / back / left / right -> pad A / B / X / Y
      const pb = new Uint8Array(3232); pb[0] = 0xA7; pb[1] = 8; pb.set(new TextEncoder().encode('acctest'), 2); pb[2801] = 0x80; pb[2815] = 0x81; pb[2829] = 0x82; pb[2843] = 0x83;
      if (!await bWrite('sc26re/rmp/3', pb)) throw new Error('test profile did not write');
      await set('remap_profile', 3, false); await set('remap_on', 1, false); if (bk.gm) await set('gm_enabled', 0, false);
      const blob = c => { const b = new Uint8Array(32), v = new DataView(b.buffer); b[0] = c.st || 0; b[1] = c.fl || 0; v.setUint16(2, c.sens || 100, true); v.setUint16(4, c.dz || 0, true); b[6] = c.btn || 0; v.setUint16(8, c.bs || 100, true); v.setUint16(10, c.gs || 100, true); (c.g || []).forEach((a, k) => { b[12 + 2 * k] = a; }); return b; };
      const GACT = [0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6]; // gestures -> stick directions (L up / down / left / right, R up / down / left)
      const gestOf = e => { const o = []; if (e.lsy > 30000) o.push('fwd'); if (e.lsy < -30000) o.push('back'); if (e.lsx < -30000) o.push('left'); if (e.lsx > 30000) o.push('right'); if (e.rsy > 30000) o.push('up'); if (e.rsy < -30000) o.push('down'); if (e.rsx < -30000) o.push('shake'); return o; };
      const at = (log, end) => { let e = null; for (const l of log) if (l.t <= end) e = l; return e; };
      { // tilt -> stick
        const ok = await bWrite('sc26re/acc/3', blob({ st: 1 })), t = [[0, 0], [22.5, 0], [0, 45], [0, -22.5], [0, 0]], want = [[0, 0], [0, 16384], [32767, 0], [-16384, 0], [0, 0]];
        const r = await runScript(t.map(([f, s]) => ({ ms: 150, ...tilt(f, s) }))), bad = [];
        t.forEach((x, i) => { const e = at(r.log, 150 * (i + 1) - 3); if (!e || Math.abs(e.lsx - want[i][0]) > 6 || Math.abs(e.lsy - want[i][1]) > 6) bad.push({ t: x, got: e && [e.lsx, e.lsy], want: want[i] }); });
        res.push({ name: 'tilt -> stick', pass: ok && !bad.length, bad });
      }
      { // tilt -> buttons, 8-way, hysteresis
        const ok = await bWrite('sc26re/acc/3', blob({ btn: 2 })), t = [[0, 0, 0], [25, 0, 1], [18.5, 0, 1], [16, 0, 0], [25, 30, 9], [-25, -25, 6], [0, 0, 0]];
        const r = await runScript(t.map(([f, s]) => ({ ms: 150, ...tilt(f, s) }))), bad = [];
        t.forEach(([f, s, w], i) => { const e = at(r.log, 150 * (i + 1) - 3), got = e ? (e.btn & 15) : -1; if (got !== w) bad.push({ t: [f, s], got, want: w }); });
        res.push({ name: 'tilt -> buttons 8-way', pass: ok && !bad.length, bad });
      }
      const jab = (axis, g) => { const base = { ax: 0, ay: 0, az: G }, a = { ...base }, b = { ...base }; a[axis] += Math.round(g * G); b[axis] -= Math.round(g * G); return [{ ms: 30, ...a }, { ms: 30, ...b }, { ms: 450, ...base }]; };
      const gcase = async (name, c, seq, want) => {
        const ok = await bWrite('sc26re/acc/3', blob(c)), r = await runScript([{ ms: 600, ax: 0, ay: 0, az: G }, ...seq]);
        const seen = []; let prev = []; for (const e of r.log) { const g = gestOf(e); for (const x of g) if (!prev.includes(x)) seen.push(x); prev = g; }
        res.push({ name, pass: ok && JSON.stringify(seen) === JSON.stringify(want), seen, want });
      };
      for (const [ax, g, w] of [['ay', 2, 'fwd'], ['ay', -2, 'back'], ['ax', -2, 'left'], ['ax', 2, 'right'], ['az', 2, 'up'], ['az', -2, 'down']]) await gcase('jab ' + w, { g: GACT }, jab(ax, g), [w]);
      await gcase('1.2 g at 100 %: nothing', { g: GACT }, jab('ay', 1.2), []);
      await gcase('200 %: 1 g fires', { g: GACT, gs: 200 }, jab('ay', 1.0), ['fwd']);
      const shake = n => { const s = []; for (let i = 0; i < n; i++) s.push({ ms: 60, ax: Math.round((i % 2 ? -2 : 2) * G), ay: 0, az: G }); return s; };
      await gcase('shake of 8 jolts: once (and the first jolt)', { g: GACT }, [...shake(8), { ms: 500, ax: 0, ay: 0, az: G }], ['right', 'shake']);
      await gcase('long shake, pause, shake: twice', { g: GACT }, [...shake(16), { ms: 1000, ax: 0, ay: 0, az: G }, ...shake(8), { ms: 500, ax: 0, ay: 0, az: G }], ['right', 'shake', 'right', 'shake']);
      const slow = []; for (let d = 0; d <= 60; d += 3) slow.push({ ms: 30, ...tilt(d, 0) });
      await gcase('slow tilt: nothing', { g: GACT }, slow, []);
      await set('gm_act_mode', 0, false); await set('gm_enabled', 1, false);
      await gcase('gyro on + pause flag: nothing', { g: GACT, fl: 0x80 }, jab('ay', 2), []);
      await gcase('gyro on, no flag: works', { g: GACT }, jab('ay', 2), ['fwd']);
    } catch (e) { err = e.message; }
    finally {
      try { await bWrite('sc26re/acc/3', bk.acc3); await bWrite('sc26re/rmp/3', bk.prof3); await set('remap_profile', bk.rp, false); await set('remap_on', bk.on, false); await set('gm_enabled', bk.gm, false); await set('gm_act_mode', bk.act, false); restored = true; } catch (e) { err = (err || '') + ' restore: ' + e.message; }
    }
    return { checks: res.length, failed: res.filter(r => !r.pass).length, err, restored, res };
  }
  // on the real, still controller: gestures at 200 % bound, the gyro range switched 10 times (each switch
  // restarts the IMU stream, which passes the sensor through other ranges): no gesture may fire
  async function settle() {
    const { rd, snd, get, set } = HWREG;
    if (!await HWREG.connect()) return 'no controller';
    const bRead = async (pre, size) => { const out = new Uint8Array(size); for (let o = 0; o < size;) { const x = await rd(pre + '/' + o.toString(16)); if (!x.length) return null; out.set(x.slice(0, size - o), o); o += x.length; } return out; };
    const bWrite = async (pre, b) => { for (let a = 0; a < 4; a++) { for (let o = 0; o < b.length; o += 40) await snd(pre + '/' + o.toString(16), 0xEE, Array.from(b.slice(o, o + 40))); await snd(pre, 0xEF); const r = await bRead(pre, b.length); if (r && r.every((v, k) => v === b[k])) return true; } return false; };
    const bk = { prof3: await bRead('sc26re/rmp/3', 3232), acc3: await bRead('sc26re/acc/3', 32), rp: await get('remap_profile'), on: await get('remap_on'), fsr: await get('gyro_fsr') };
    const ev = []; const H = e => { if (e.reportId !== 0x42 || e.data.byteLength < 17) return; const d = e.data, v = [9, 11, 13, 15].map(o => d.getInt16(o, true)); if (v.some(x => Math.abs(x) > 30000)) ev.push([Math.round(performance.now()), v]); };
    const ds = await navigator.hid.getDevices(); let quiet = 0, err = null;
    try {
      const pb = new Uint8Array(3232); pb[0] = 0xA7; pb[1] = 8; pb.set(new TextEncoder().encode('settle'), 2); await bWrite('sc26re/rmp/3', pb);
      const a = new Uint8Array(32), v = new DataView(a.buffer); v.setUint16(10, 200, true); [0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6].forEach((x, k) => a[12 + 2 * k] = x); await bWrite('sc26re/acc/3', a);
      await set('remap_profile', 3, false); await set('remap_on', 1, false); await sleep(1500);
      ds.forEach(x => x.addEventListener('inputreport', H)); await sleep(5000); quiet = ev.length;
      for (let i = 0; i < 10; i++) { await set('gyro_fsr', i % 2 ? 250 : 2000, false); await sleep(1500); }
    } catch (e) { err = e.message; }
    finally { ds.forEach(x => x.removeEventListener('inputreport', H)); try { await set('gyro_fsr', bk.fsr, false); await bWrite('sc26re/acc/3', bk.acc3); await bWrite('sc26re/rmp/3', bk.prof3); await set('remap_profile', bk.rp, false); await set('remap_on', bk.on, false); } catch (e) { err = (err || '') + ' restore: ' + e.message; } }
    return { quietGestures: quiet, gesturesDuringRangeSwitches: ev.length - quiet, pass: !err && ev.length === 0, err };
  }
  window.ACCTEST = { run, settle };
})();
