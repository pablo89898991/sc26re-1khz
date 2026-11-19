// Save writes every changed profile: copy profile 1 into 3, change 1, save once, reload from the controller
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
let bad = 0; const check = (n, ok, i = '') => { if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + n + (i ? ' — ' + i : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  // mark profile 1 with a name, copy it to 3
  await pg.evaluate(() => { prof[0][2] = 0x5A; prof[0][3] = 0x51; setDirty(); });
  const btn = await pg.$('[data-pcopy="2"]');
  if (!btn) { await pg.evaluate(() => { prof[2] = prof[0].slice(); setDirty(true, 2); }); }
  else await btn.click();
  const chip = await pg.textContent('#dirty');
  check('the chip names both profiles', /1/.test(chip) && /3/.test(chip), chip);
  await pg.click('#write');
  await pg.waitForFunction(() => /saved|differs|err/.test(document.querySelector('#pstatus').innerHTML), null, { timeout: 60000 });
  check('save wrote both', /profile 1, 3 saved/.test(await pg.textContent('#pstatus')), await pg.textContent('#pstatus'));
  const p3 = await pg.evaluate(async () => Array.from(await readProfile(2)));
  check('profile 3 on the controller is the copy', p3[2] === 0x5A && p3[3] === 0x51, p3.slice(0, 6).join(','));
  check('no unsaved changes left', !(await pg.evaluate(() => dirty)));
  check('no page errors', !errs.length, errs.join(' | '));
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e); process.exit(1); });
