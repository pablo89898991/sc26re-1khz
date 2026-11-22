// SC2 Overlay app update check: the overlay page (app mode) reads sc26re/version from the
// controller and hands it to the app (window.sc26reApp.firmware). Needs sim_server.py and web on 8766.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  const pg = await br.newPage();
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.addInitScript(() => { window.__fw = []; window.sc26reApp = { edit() {}, arrange() {}, firmware: i => window.__fw.push(i) }; });
  await pg.goto('http://127.0.0.1:8766/overlay.html?app=1&pip=1');
  await pg.evaluate(() => document.querySelector('#connect').click());
  await pg.waitForFunction(() => window.__fw.length > 0, null, { timeout: 20000 }).catch(() => {});
  await new Promise(r => setTimeout(r, 7000)); /* only once per connection */
  const fw = await pg.evaluate(() => window.__fw);
  const ok = fw.length === 1 && fw[0].release === 'v51.2' && fw[0].build === 0xF05501B4 && !errs.length;
  console.log((ok ? 'PASS' : 'FAIL') + ' firmware version reported once — ' + JSON.stringify(fw) + ' ' + errs.join('; '));
  await br.close(); process.exit(ok ? 0 : 1);
})();
