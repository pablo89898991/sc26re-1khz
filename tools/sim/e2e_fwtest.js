// Firmware test page (web/fwtest.html) against the virtual controller: runs to the end without page
// errors, every setting check passes (ranges, clamping, auto, put back), settings are as before.
// Rates and IMU checks need the real controller (the virtual one sends 200 reports/s, no IMU).
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
let bad = 0, total = 0;
const check = (name, ok, info = '') => { total++; if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage();
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/fwtest.html');
  await pg.click('#connect');
  check('connects', await pg.waitForFunction(() => !document.querySelector('#run').disabled, null, { timeout: 15000 }).then(() => true, () => false), await pg.textContent('#status'));
  await pg.click('#run');
  const done = await pg.waitForFunction(() => window.fwtResults, null, { timeout: 900000 }).then(() => true, () => false);
  const R = await pg.evaluate(() => window.fwtResults);
  check('runs to the end', done && R && !R.error, R && R.error);
  const set = R.checks.filter(c => c.name.startsWith('setting '));
  check('settings: all checked and all fine', set.length > 60 && set.every(c => c.ok), set.filter(c => !c.ok).map(c => c.name + ': ' + c.info).join(' | ').slice(0, 600));
  check('pad report check ran', R.checks.some(c => c.name.startsWith('pad report')), '');
  check('no page errors', errs.length === 0, errs.join('; '));
  console.log(`${total} checks, ${bad} failed`);
  await br.close(); process.exit(bad ? 1 : 0);
})();
