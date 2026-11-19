// Remapper page, "Controller" and "Gyro" views against the virtual controller: every setting shows
// the firmware's value (also Valve's own boot chime level, which comes without the sc26re tag),
// saving changes only what was changed. Needs a fresh sim_server.py on 8765. V=1 prints every check.
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
async function chime() { const r = Buffer.from((await feature(0xED, 'user/haptic_boot_level')).rsp, 'hex'); return r[1] >= 1 ? r[2] : null; }
async function cfg(name, v) { await feature(0xEE, 'sc26re/' + name, [v & 255, (v >> 8) & 255]); await feature(0xEF, 'sc26re/' + name); }
const AUTO = -32768;

(async () => {
  await cfg('trig_deadzone', 11); await cfg('led_brightness', 40); await cfg('gm_sens', 175); await cfg('gyro_fsr', 500);
  await feature(0xEE, 'user/haptic_boot_level', [1]);
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  check('connects and loads', await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 }).then(() => true, () => false));
  await pg.click('[data-v="ctl"]');
  await pg.waitForFunction(() => /read|err/.test(document.querySelector('#lvStatus').textContent), null, { timeout: 60000 }).catch(() => {});
  check('controller settings read', /read/.test(await pg.textContent('#lvStatus')), await pg.textContent('#lvStatus'));
  check('the boot chime shows the controller\'s level (quiet)', (await pg.inputValue('#lv_chime')) === '1', await pg.inputValue('#lv_chime'));
  const names = await pg.evaluate(() => LV.flatMap(([, l]) => l.map(([n]) => n)).filter(n => n !== 'chime' && !n.startsWith('cap:')));
  let wrong = [];
  for (const n of names) { const v = await fw(n); const [val, au] = await pg.evaluate(n => [document.getElementById('lv_' + n).value, document.getElementById('lvau_' + n).checked], n);
    if (v === AUTO ? !au : +val !== v) wrong.push(`${n}: page ${au ? 'Steam' : val}, firmware ${v}`); }
  check('every controller setting shows the firmware value', !wrong.length, wrong.slice(0, 4).join(' | '));
  /* save without changing anything: nothing changes (the chime above all) */
  const before = {}; for (const n of names) before[n] = await fw(n);
  await pg.click('#lvSave'); await pg.waitForFunction(() => /saved|err/.test(document.querySelector('#lvStatus').textContent), null, { timeout: 30000 }).catch(() => {});
  wrong = []; for (const n of names) { const v = await fw(n); if (v !== before[n]) wrong.push(`${n}: ${before[n]} -> ${v}`); }
  check('saving unchanged settings changes nothing', !wrong.length && (await chime()) === 1, wrong.join(' | ') + ' chime ' + (await chime()));
  /* a change goes through */
  await pg.fill('#lv_trig_deadzone', '17'); await pg.selectOption('#lv_chime', '2'); await pg.click('#lvSave');
  await sleep(1500);
  check('a changed setting and the chime are saved', (await fw('trig_deadzone')) === 17 && (await chime()) === 2, `${await fw('trig_deadzone')}, chime ${await chime()}`);
  /* gyro view: the same */
  await pg.click('[data-v="gyro"]');
  await pg.waitForFunction(() => /read|err/.test(document.querySelector('#gyStatus').textContent), null, { timeout: 30000 }).catch(() => {});
  check('gyro settings show the firmware values', (await pg.inputValue('#g_gm_sens')) === '1.75' && (await pg.inputValue('#g_gyro_fsr')) === '500',
    `${await pg.inputValue('#g_gm_sens')}, ${await pg.inputValue('#g_gyro_fsr')}`);
  /* a page opened before the controller has a chime: Save without reading leaves it alone */
  { const p2 = await br.newPage({ viewport: { width: 1300, height: 1000 } }); p2.on('pageerror', e => errs.push(e.message)); p2.on('dialog', d => d.accept());
    await p2.addInitScript({ path: __dirname + '/webhid_shim.js' }); await p2.goto('file://' + __dirname + '/../../web/remap.html');
    await p2.evaluate(async () => { const [d] = await navigator.hid.requestDevice({ filters: [] }); await d.open(); devs.push(d); });
    await p2.evaluate(() => lvWrite(true)); await sleep(500);
    check('save before reading leaves the chime alone', (await chime()) === 2, String(await chime())); await p2.close(); }
  /* a firmware without the remapper (before v51): the page says so (it used to report "a reply for another request") */
  { for (const n of ['remap_on', 'remap_profile']) await post('/block', { path: 'sc26re/' + n, ms: 30000 });
    const p3 = await br.newPage({ viewport: { width: 1300, height: 1000 } }); p3.on('pageerror', e => errs.push(e.message)); p3.on('dialog', d => d.accept());
    await p3.addInitScript({ path: __dirname + '/webhid_shim.js' }); await p3.goto('file://' + __dirname + '/../../web/remap.html');
    const t0 = Date.now(); await p3.click('#connect');
    const ok = await p3.waitForFunction(() => /no remapper/.test(document.querySelector('#status').textContent), null, { timeout: 20000 }).then(() => true, () => false);
    check('older firmware: "this firmware has no remapper", quickly', ok && Date.now() - t0 < 8000, `${await p3.textContent('#status')} after ${Date.now() - t0} ms`);
    await p3.close(); for (const n of ['remap_on', 'remap_profile']) await post('/block', { path: 'sc26re/' + n, ms: 0 }); }
  check('no page errors', !errs.length, errs.slice(0, 3).join(' | '));
  console.log(`${total} checks, ${bad} failed`);
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e.stack || e.message); process.exit(1); });
