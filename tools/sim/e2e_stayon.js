// Remapper: "Keep controller on" (as in the report meter): the keep-alive every 2 s while ticked, remembered.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
let bad = 0; const check = (n, c, i = '') => { if (!c) bad++; console.log((c ? 'PASS ' : 'FAIL ') + n + (c ? '' : ' — ' + i)); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const ctx = await br.newContext(); const pg = await ctx.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.addInitScript(() => { try { if (!sessionStorage.getItem('t')) { sessionStorage.setItem('t', '1'); localStorage.removeItem('sc26re_stayon'); } } catch (e) {} });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  check('ticked by default', await pg.evaluate(() => document.getElementById('stayOn').checked));
  await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(4000);
  await pg.evaluate(() => { window.__aw = 0; const r = read; read = (p, ...a) => { if (p === 'sc26re/awake') window.__aw++; return r(p, ...a); }; });
  await pg.waitForTimeout(6100); const on = await pg.evaluate(() => window.__aw);
  check('keep-alive every 2 s while ticked', on >= 3 && on <= 4, on);
  await pg.evaluate(() => { const c = document.getElementById('stayOn'); c.checked = false; c.onchange({ target: c }); window.__aw = 0; });
  await pg.waitForTimeout(4100); check('none when unticked', (await pg.evaluate(() => window.__aw)) === 0);
  await pg.reload(); await pg.waitForTimeout(1000);
  check('remembered after a reload', !(await pg.evaluate(() => document.getElementById('stayOn').checked)));
  await pg.evaluate(() => { const c = document.getElementById('stayOn'); c.checked = true; c.onchange({ target: c }); });
  check('no page errors', !errs.length, errs.join('; '));
  await br.close(); console.log(bad ? `${bad} failed` : 'all passed'); process.exit(bad ? 1 : 0);
})();
