// Hardware regression check, run in the remapper page (remap.html) on the user's controller.
// It goes through everything that touches the gyro set-up and, after each, checks the sensor and the
// firmware agree: range register = set range, conversion = set range, mode / ODR registers = settings.
// Nothing is saved that was not there before: every change is put back. Usage: paste, then
//   HWREG.run().then(r => console.log(r))
(() => {
  const T = (p, ms) => Promise.race([p, new Promise((_, j) => setTimeout(() => j(new Error('timeout')), ms))]);
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  let D = null;
  async function connect() {
    for (const x of await navigator.hid.getDevices()) {
      try { if (!x.opened) await T(x.open(), 3000); await T(x.sendFeatureReport(1, pathCmd(0xED, 'sc26re/gm_sens')), 1000); D = x; return true; } catch (e) {}
    }
    return false;
  }
  async function rd(path) {
    const tag = pathTag(path);
    for (let n = 0; n < 15; n++) { // another page polling the controller (the meter) can take the reply
      try { await T(D.sendFeatureReport(1, pathCmd(0xED, path)), 800); } catch (e) { await sleep(60 + 20 * n); continue; }
      for (let k = 0; k < 12; k++) {
        await sleep(20);
        try {
          const r = new Uint8Array((await T(D.receiveFeatureReport(1), 800)).buffer);
          const o = r[0] === 0xED ? 0 : (r[1] === 0xED ? 1 : -1); if (o < 0) continue;
          const x = r.slice(o + 2, o + 2 + r[o + 1]); if (!x.length) return x; if (x[0] !== tag) continue; return x.slice(1);
        } catch (e) {}
      }
    }
    throw new Error('read failed ' + path);
  }
  async function snd(path, op, data = []) {
    for (let n = 0; n < 5; n++) { try { await T(D.sendFeatureReport(1, pathCmd(op, path, data)), 800); await sleep(8); return; } catch (e) { await sleep(60); } }
    throw new Error('send failed ' + path);
  }
  const s16 = b => (b[0] | b[1] << 8) << 16 >> 16;
  const get = async k => s16(await rd('sc26re/' + k));
  // a setting: staged and saved, read back until it is there (a send right after a flash write can be lost)
  async function set(k, v, save = true) {
    for (let a = 0; a < 4; a++) {
      await snd('sc26re/' + k, 0xEE, [v & 255, (v >> 8) & 255]); if (save) await snd('sc26re/' + k, 0xEF);
      if (await get(k) === v) return;
    }
    throw new Error('setting did not stick: ' + k + ' = ' + v);
  }
  const ODR = { 2000: 0xA, 4000: 0xB, 8000: 0xC }, FS = { 250: 1, 500: 2, 1000: 3, 2000: 4 };
  async function state() {
    await rd('sc26re/imumode'); await sleep(80);
    const b = await rd('sc26re/imumode'), v = new DataView(b.buffer, b.byteOffset);
    return { ctrl2: b[11], ctrl6: b[15], haodr: b[18], ndps: v.getUint32(6, true), fixes: v.getUint16(44, true), mode: b[27] };
  }
  // what the sensor must look like for the settings (and the profile's own range, when it has one)
  async function check(name, R) {
    await sleep(400); // the fast loop applies within a few ms; the 100 ms range check twice
    const s = await state(), fsr = await expectFsr(), mode = await get('gyro_mode'), odr = await get('gyro_odr');
    const code = FS[fsr], want = 4375000 * (1 << code);
    const P = [];
    if ((s.ctrl6 & 15) !== code) P.push(`range register ${s.ctrl6 & 15}, set ${code} (${fsr} dps)`);
    if (s.ndps !== want) P.push(`conversion ${s.ndps / 1e6} mdps, set range needs ${want / 1e6}`);
    if ((s.ctrl2 & 15) !== ODR[odr]) P.push(`gyro ODR code ${s.ctrl2 & 15}, set ${ODR[odr]} (${odr} Hz)`);
    if (((s.ctrl2 >> 4) & 7) !== (mode ? 0 : 1)) P.push(`gyro mode bits ${(s.ctrl2 >> 4) & 7}, mode ${mode}`);
    R.push({ name, ok: !P.length, problems: P, fixes: s.fixes }); window.HWREG.progress = R;
  }
  let profFsr = {};
  async function expectFsr() {
    const on = await get('remap_on'), p = await get('remap_profile');
    if (on && profFsr[p]) return profFsr[p];
    return await get('gyro_fsr');
  }
  // the gyro page's Save / Apply, as the page does it (every gyro setting written back)
  async function gyroPageWrite(save) {
    for (const [n, , kind] of GY) {
      if (n === '#') continue;
      const keys = kind === 'band' ? [`gyro_band_${n.slice(5)}_lo`, `gyro_band_${n.slice(5)}_hi`]
        : kind === 'gband' ? [`gm_band_${n.slice(4)}_lo`, `gm_band_${n.slice(4)}_hi`]
        : n === 'cal360' ? ['gm_cal_hi', 'gm_cal_lo'] : [n];
      for (const k of keys) { const v = await get(k); await snd('sc26re/' + k, 0xEE, [v & 255, (v >> 8) & 255]); if (save) await snd('sc26re/' + k, 0xEF); }
    }
  }
  async function run() {
    if (!await connect()) return 'no controller';
    const R = [], keep = {};
    for (const k of ['gyro_fsr', 'gyro_mode', 'gyro_odr', 'remap_on', 'remap_profile', 'gm_enabled']) keep[k] = await get(k);
    // which profiles have their own range (flags bit 1, int16 at 124)
    for (let p = 0; p < 4; p++) {
      const b = await rd(`sc26re/rmp/${p}/78`), b2 = await rd(`sc26re/rmp/${p}/0`);
      const fl = b2[17], f = s16(b.slice(4, 6)); // gyro_fsr at byte 124 of the profile
      if (b2[0] === 0xA7 && (fl & 2) && f) profFsr[p] = f;
    }
    try {
      await check('as found', R);
      await gyroPageWrite(true); await check('gyro page Save', R);
      await gyroPageWrite(true); await check('gyro page Save again', R);
      await gyroPageWrite(false); await check('gyro page Apply', R);
      for (const f of [500, 1000, 2000, 250, keep.gyro_fsr]) { await set('gyro_fsr', f, false); await check(`range ${f} (live)`, R); }
      for (const o of [4000, 8000, 2000, keep.gyro_odr]) { await set('gyro_odr', o, false); await check(`ODR ${o} (live)`, R); }
      for (const m of [1 - keep.gyro_mode, keep.gyro_mode]) { await set('gyro_mode', m, false); await check(`sensor mode ${m} (live)`, R); }
      await set('gyro_mode', 1 - keep.gyro_mode, false); await gyroPageWrite(false); await check('other mode, then Apply', R);
      await set('gyro_mode', keep.gyro_mode, false);
      for (const on of [1 - keep.remap_on, keep.remap_on]) { await set('remap_on', on, false); await check(`remapper ${on ? 'on' : 'off'}`, R); }
      await set('remap_on', 1, false);
      for (const p of [0, 1, 2, 3, keep.remap_profile]) { await set('remap_profile', p, false); await check(`remapper on, profile ${p + 1}${profFsr[p] ? ' (own range ' + profFsr[p] + ')' : ''}`, R); }
      await set('remap_on', keep.remap_on, false);
      for (const on of [1 - keep.gm_enabled, keep.gm_enabled]) { await set('gm_enabled', on, false); await check(`gyro mouse ${on ? 'on' : 'off'}`, R); }
      await gyroPageWrite(true); await check('gyro page Save at the end', R);
    } finally {
      for (const [k, v] of Object.entries(keep)) { try { if (await get(k) !== v) await set(k, v, false); } catch (e) {} }
    }
    const bad = R.filter(r => !r.ok);
    return { checks: R.length, failed: bad.length, bad, fixesFirst: R[0].fixes, fixesLast: R[R.length - 1].fixes };
  }
  window.HWREG = { run, connect, rd, snd, get, set, state };
})();
