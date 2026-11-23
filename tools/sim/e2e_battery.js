// Battery badge (sc26re/battery) on the report meter, the remapper and the overlay (+ app tooltip hook).
// Needs sim_server.py and web on 8766. The simulator reports 76 %, charging, 4.052 V, 500 mA, 24.5 C.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  let fail = 0;
  for (const [page, q] of [['meter.html', ''], ['remap.html', ''], ['overlay.html', '?app=1&pip=1']]) {
    const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
    await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await pg.addInitScript(() => { window.__bat = []; window.sc26reApp = { edit() {}, arrange() {}, firmware() {}, battery: i => window.__bat.push(i) }; });
    await pg.goto('http://127.0.0.1:8766/' + page + q);
    await pg.evaluate(() => document.querySelector('#connect').click());
    await pg.waitForFunction(() => { const b = document.getElementById('batt'); return b && !b.hidden && b.textContent; }, null, { timeout: 40000 }).catch(() => {});
    const r = await pg.evaluate(() => { const b = document.getElementById('batt'); return { text: b && b.textContent, title: b && b.title, hidden: b && b.hidden, app: window.__bat.filter(Boolean)[0] }; });
    let ok = r.text === '⚡ 76%' && /Battery 76% · charging · 4\.05 V · 500 mA · 24\.5 °C/.test(r.title) && !errs.length;
    if (page === 'overlay.html') ok = ok && r.app && r.app.pct === 76 && r.app.charging === true;
    if (!ok) fail++;
    console.log((ok ? 'PASS ' : 'FAIL ') + page + ' ' + JSON.stringify(r) + (errs.length ? ' errors: ' + errs.join('; ') : ''));
    await pg.close();
  }
  await br.close(); process.exit(fail ? 1 : 0);
})();
