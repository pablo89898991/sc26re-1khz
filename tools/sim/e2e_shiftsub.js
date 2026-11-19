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
  // v8: the pad click has its own binding while the pad is shifted
  await pg.click('#v_map [data-s="26"]');
  await pg.click('#dlg .seg [data-l="1"]');
  const t26 = await pg.textContent('#dlg h4');
  await pg.selectOption('#dlg [data-gi="0:0"]', '5');
  await pg.click('#dlgDone');
  const [cn, cs, ct] = await pg.evaluate(() => [prof[cur][488 + 26 * 14 + 1], prof[cur][SCLICK + 2 * 14 + 1], document.querySelector('#v_map [data-s="26"] .b').textContent]);
  check('the shifted pad click is its own binding (profile v8)', /shifted/.test(t26) && cn === 0 && cs === 5 && /⇧ L5: B/.test(ct), [t26, cn, cs, ct].join(' / '));
  // turning a shift on: the click keeps what it did (its normal binding is copied)
  await pg.evaluate(() => { prof[cur][488 + 15 * 14 + 1] = 6; render(); });
  await pg.click('#v_map [data-s="ls"]'); await pg.selectOption('#shb', { label: 'L4' }); await pg.click('#dlgDone');
  const c15 = await pg.evaluate(() => prof[cur][SCLICK + 1]);
  check('a new stick shift starts with the click it had', c15 === 6, c15);
  // tester: cycle showed only the press row (its other sets hidden, also stale ones it still used)
  await pg.click('#v_map [data-s="0"]');
  await pg.check('#dlg [data-bit="64"]');
  const rows = await pg.$$eval('#dlg .grp > span', x => x.map(e => e.textContent));
  const cb = await pg.$$eval('#dlg [data-bit]', x => x.map(e => e.dataset.bit));
  check('cycle shows its three sets and only the cycle mode', rows.join() === 'set 1,set 2,set 3' && cb.join() === '64', rows.join() + ' / ' + cb.join());
  // tester: the delay box went up by 1 ms, the firmware keeps 10 ms steps
  await pg.evaluate(() => { const d = document.querySelector('#dlg details.btime'); if (d) d.open = true; });
  const step = await pg.getAttribute('#bdst_n', 'step');
  await pg.fill('#bdst_n', '15'); await pg.dispatchEvent('#bdst_n', 'change');
  const v = await pg.inputValue('#bdst_n');
  check('delay start steps by 10 ms and snaps to them', step === '10' && v === '20', step + ' ' + v);
  await pg.click('#dlgDone');
  check('no page errors', !errs.length, errs.join(' | '));
  await br.close();
  console.log(results.length + ' checks, ' + results.filter(x => !x).length + ' failed');
  process.exit(results.every(x => x) ? 0 : 1);
})();
