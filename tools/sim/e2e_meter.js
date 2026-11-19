// Report meter (web/meter.html) against the virtual controller: connects, counts the reports,
// reads the live settings and the gyro mouse settings exactly as the firmware has them, applies
// and saves changes, restores defaults; no page errors. A path the firmware does not answer
// (an older firmware) must not make the page treat every later reply as untagged.
// Needs a fresh sim_server.py on 8765. V=1 prints every check.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const SRV = 'http://127.0.0.1:8765';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: typeof b === 'string' ? b : JSON.stringify(b) }).then(r => r.json());
let bad = 0, total = 0;
const check = (name, ok, info = '') => { total++; if (!ok) bad++; if (!ok || process.env.V) console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); return ok; };
async function feature(op, path, data = []) {
  const p = Buffer.from(path), req = Buffer.alloc(64); req[0] = op; req[1] = p.length + 1 + data.length; p.copy(req, 2); Buffer.from(data).copy(req, 3 + p.length);
  return post('/feature', req.toString('hex'));
}
async function fw(name) { const r = Buffer.from((await feature(0xED, 'sc26re/' + name)).rsp, 'hex'); return r[1] >= 3 ? r.readInt16LE(3) : null; }
async function cfg(name, v) { await feature(0xEE, 'sc26re/' + name, [v & 255, (v >> 8) & 255]); await feature(0xEF, 'sc26re/' + name); }
const AUTO = -32768;

(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/meter.html');
  await cfg('trig_deadzone', 9); await cfg('gyro_fsr', 1000); await cfg('gm_sens', 150); await cfg('trig_click', 60);
  await pg.click('#connect');
  check('connects', await pg.waitForFunction(() => /connected/.test(document.querySelector('#status').textContent), null, { timeout: 10000 }).then(() => true, () => false), await pg.textContent('#status'));
  await sleep(2500);
  const rate = await pg.textContent('#sRate');
  check('counts the pad reports', /\d/.test(rate) && +rate.replace(/[^\d.]/g, '') > 50, rate);
  /* live settings: the page shows what the firmware has */
  await pg.click('#lsRead');
  check('reads the live settings', await pg.waitForFunction(() => /read|err|fail/.test(document.querySelector('#lsStatus').textContent), null, { timeout: 30000 }).then(() => true, () => false) && /read/.test(await pg.textContent('#lsStatus')), await pg.textContent('#lsStatus'));
  const names = await pg.evaluate(() => LS.map(([n]) => n).filter(n => !/^(band|cap):/.test(n)));
  let wrong = [];
  for (const n of names) {
    const v = await fw(n); if (v === null) { wrong.push(n + ': firmware has none'); continue; }
    const [shown, auto] = await pg.evaluate(n => [document.getElementById('ls_' + n).value, document.getElementById('lsauto_' + n).checked], n);
    if (v === AUTO ? !auto : +shown !== v) wrong.push(`${n}: page ${auto ? 'auto' : shown}, firmware ${v}`);
  }
  check('every live setting shows the firmware value', !wrong.length, wrong.slice(0, 4).join(' | '));
  /* apply: only the changed ones, until power-off; then save */
  await pg.evaluate(() => { const set = (n, v) => { const e = document.getElementById('ls_' + n); e.value = v; e.dispatchEvent(new Event('input')); e.dispatchEvent(new Event('change')); };
    set('trig_deadzone', 14); set('gyro_fsr', 500); const a = document.getElementById('lsauto_trig_click'); a.checked = true; a.dispatchEvent(new Event('change')); });
  await pg.click('#lsApply');
  await pg.waitForFunction(() => /applied|failed/.test(document.querySelector('#lsStatus').textContent), null, { timeout: 30000 }).catch(() => {});
  check('apply changes the firmware', (await fw('trig_deadzone')) === 14 && (await fw('gyro_fsr')) === 500 && (await fw('trig_click')) === AUTO,
    `${await pg.textContent('#lsStatus')}: ${await fw('trig_deadzone')}, ${await fw('gyro_fsr')}, ${await fw('trig_click')}`);
  check('apply leaves the others alone', (await fw('gm_sens')) === 150);
  /* the gyro mouse panel */
  await pg.click('#gmRead'); await sleep(1500);
  check('gyro mouse sensitivity shows the firmware value', Math.abs(+(await pg.inputValue('#gmSens')) - 1.5) < 1e-9, await pg.inputValue('#gmSens'));
  await pg.fill('#gmSens', '2.25'); await pg.click('#gmApply');
  check('gyro mouse apply', await pg.waitForFunction(() => true).then(async () => { for (let i = 0; i < 40; i++) { if ((await fw('gm_sens')) === 225) return true; await sleep(100); } return false; }), String(await fw('gm_sens')));
  /* a path this firmware does not answer (an older firmware has fewer) must not switch tags off */
  const after = await pg.evaluate(async () => { try { await featRead(ctl(), 'sc26re/no_such_thing'); } catch (e) {} return [ctl().__tagged, await readS16('sc26re/gyro_fsr')]; });
  check('an unknown path does not switch the reply tags off', after[0] !== false && after[1] === 500, JSON.stringify(after));
  /* the same as the very first read of a page (before any tagged reply): an older firmware without some path */
  { const p2 = await br.newPage(); p2.on('pageerror', e => errs.push(e.message));
    await p2.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await p2.goto('file://' + __dirname + '/../../web/meter.html');
    const r = await p2.evaluate(async () => { const [d] = await navigator.hid.requestDevice({ filters: [] }); await d.open(); d.__send = d.sendFeatureReport.bind(d);
      const t0 = performance.now(); const e = await featRead(d, 'sc26re/no_such_thing'); const ms = performance.now() - t0;
      const b = await featRead(d, 'sc26re/gyro_fsr'); return [e.length, Math.round(ms), d.__tagged, b[0] | (b[1] << 8)]; });
    check('a first read of an unknown path: empty, quickly, tags stay on', r[0] === 0 && r[1] < 3000 && r[2] !== false && r[3] === 500, JSON.stringify(r)); await p2.close(); }
  check('no page errors', !errs.length, errs.slice(0, 3).join(' | '));
  console.log(`${total} checks, ${bad} failed`);
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e.stack || e.message); process.exit(1); });
