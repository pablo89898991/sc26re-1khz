// Battery badge with the Puck's extra interfaces in front of the controller's: how soon it shows.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  let fail = 0;
  for (const [page, q] of [['meter.html', ''], ['remap.html', ''], ['overlay.html', '?app=1&pip=1']]) {
    const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
    await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await pg.addInitScript({ path: __dirname + '/webhid_shim_puck.js' });
    await pg.addInitScript(() => { window.sc26reApp = { edit() {}, arrange() {}, firmware() {}, battery() {} }; });
    await pg.goto('http://127.0.0.1:8766/' + page + q);
    const tc = Date.now(); await pg.evaluate(() => document.querySelector('#connect') && document.querySelector('#connect').click());
    await pg.waitForFunction(() => { const b = document.getElementById('batt'); return b && !b.hidden && /\d+%/.test(b.textContent); }, null, { timeout: 30000 }).catch(() => {});
    const ms = Date.now() - tc;
    const t = await pg.evaluate(() => document.getElementById('batt') && document.getElementById('batt').textContent);
    const ok = /76%/.test(t || '') && !errs.length;
    if (!ok) fail++;
    console.log((ok ? 'PASS ' : 'FAIL ') + page + ' battery after ' + ms + ' ms: ' + t + (errs.length ? ' errors: ' + errs.join('; ') : ''));
    await pg.close();
  }
  await br.close(); process.exit(fail ? 1 : 0);
})();
