// 3D overlay against the virtual controller: native reads work, a pressed button shows, no page errors.
// Needs sim_server.py on 8765 and the web folder served on 8766 (modules do not load from file://).
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const SRV = 'http://127.0.0.1:8765';
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: JSON.stringify(b) }).then(r => r.json());
const sleep = ms => new Promise(r => setTimeout(r, ms));
let bad = 0; const check = (n, ok, i = '') => { if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + n + (i ? ' — ' + i : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  const pg = await br.newPage({ viewport: { width: 900, height: 700 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('console', m => { if (m.type() === 'error') errs.push(m.text() + ' @' + JSON.stringify(m.location())); });
  pg.on('response', r => { if (r.status() >= 400) errs.push('HTTP ' + r.status() + ' ' + r.url()); });
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/overlay.html#debug');
  await pg.waitForFunction(() => window.__ov && window.__ov.parts && window.__ov.parts.size > 5, null, { timeout: 60000 }).catch(() => {});
  check('model loaded', await pg.evaluate(() => window.__ov && window.__ov.parts.size), '');
  await pg.click('#connect');
  await sleep(2500);
  const raw = await pg.evaluate(() => window.__ov.raw);
  check('native reads', raw[0] === true, JSON.stringify(raw));
  await post('/set', { A: 1 }); await sleep(600);
  const btnA = await pg.evaluate(() => window.__ov.st.btn);
  await post('/set', { A: 0, lsx: 30000 }); await sleep(600);
  const ls = await pg.evaluate(() => window.__ov.st.ls);
  await post('/set', { lsx: 0 });
  check('A pressed shows', (btnA & 1) === 1, String(btnA));
  check('left stick shows', ls[0] > 0.8, JSON.stringify(ls));
  // with the remapper on (a profile bound A to a key) the overlay still shows the physical button
  await post('/set', { B: 1 }); await sleep(600);
  check('B pressed shows', ((await pg.evaluate(() => window.__ov.st.btn)) & 2) === 2);
  await post('/set', { B: 0 });
  const real = errs.filter(e => !/favicon\.ico/.test(e)); check('no page errors', !real.length, real.slice(0, 4).join(' | '));
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e.message); process.exit(1); });
