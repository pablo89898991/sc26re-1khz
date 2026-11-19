// remap.html: an input that exists only in a stick / pad's shifted mode (tester, v51 test 48: a radial
// slice under "Mode shift" picked from the overview edited the normal binding, which does nothing there)
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const results = [];
const check = (name, ok, info = '') => { results.push(ok); console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  // left trackpad: scroll wheel; shifted (L5): radial menu
  await pg.click('#v_map [data-s="lp"]');
  await pg.selectOption('#amode', '4');
  await pg.selectOption('#shb', { label: 'L5' });
  await pg.selectOption('#amode', '10');
  await pg.click('#dlgDone');
  // "Slice 1" (left pad source 38) from the overview
  await pg.click('#v_map [data-s="38"]');
  const title = await pg.textContent('#dlg h4');
  const seg = await pg.$$eval('#dlg .seg button', b => b.map(x => x.textContent + (x.disabled ? ':off' : ':on')));
  check('a slice only the shifted mode has opens its shifted binding', /shifted/.test(title), title);
  check('the normal one cannot be picked for it', seg.join() === 'Normal:off,Shifted ⇧ L5:on', seg.join());
  const bits = await pg.$$eval('#dlg [data-bit]', x => x.map(e => e.dataset.bit));
  check('a radial slice offers only "release press" (it is a tap)', bits.join() === '16' && /is a tap/.test(await pg.textContent('#dlg')), bits.join());
  await pg.selectOption('#dlg [data-gi="0:0"]', '4');
  await pg.click('#dlgDone');
  const [n, sh, txt] = await pg.evaluate(() => [prof[cur][bOffL(38, 0) + 1], prof[cur][bOffL(38, 1) + 1], document.querySelector('#v_map [data-s="38"] .b').textContent]);
  check('the key went to the shifted binding', n === 0 && sh === 4, n + ' ' + sh);
  check('the overview shows it as the shifted one', /⇧ L5: A/.test(txt) && !/^—/.test(txt.trim()), txt);
  // a normal binding left on it from before is not shown as if it did something
  await pg.evaluate(() => { prof[cur][bOffL(38, 0) + 1] = 5; render(); });
  const txt2 = await pg.textContent('#v_map [data-s="38"] .b');
  check('a normal binding the mode does not have is not shown', !/B/.test(txt2.replace('⇧', '')), txt2);
  // an input both modes have (the click) still opens the normal one from the overview
  await pg.click('#v_map [data-s="26"]');
  check('the pad click (not part of the modes) opens as before', !/shifted/.test(await pg.textContent('#dlg h4')));
  await pg.click('#dlgDone');
  check('no page errors', !errs.length, errs.join(' | '));
  await br.close();
  console.log(results.length + ' checks, ' + results.filter(x => !x).length + ' failed');
  process.exit(results.every(x => x) ? 0 : 1);
})();
