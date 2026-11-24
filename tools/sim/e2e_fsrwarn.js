// Remapper: warning when a profile's own gyro range differs from the global one.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  await pg.evaluate(() => document.querySelector('#connect').click());
  await pg.waitForTimeout(4000);
  const r = await pg.evaluate(() => {
    const out = {}; const el = document.getElementById('gyFsrWarn');
    out.globRead = gyGlobFsr;
    for (let i = 1; i < 4; i++) prof[i][17] &= ~2;
    gyGlobFsr = 250; const b = prof[0]; b[17] |= 2; new DataView(b.buffer).setInt16(124, 2000, true); gyFsrWarn();
    out.diffShown = !el.hidden; out.text = el.textContent.slice(0, 90);
    new DataView(b.buffer).setInt16(124, 250, true); gyFsrWarn(); out.sameHidden = el.hidden;
    b[17] &= ~2; new DataView(b.buffer).setInt16(124, 2000, true); gyFsrWarn(); out.noOwnHidden = el.hidden;
    return out; });
  const ok = r.globRead && r.diffShown && r.sameHidden && r.noOwnHidden && !errs.length;
  console.log((ok ? 'PASS ' : 'FAIL ') + JSON.stringify(r) + (errs.length ? ' errors: ' + errs.join('; ') : ''));
  await br.close(); process.exit(ok ? 0 : 1);
})();
