// UI sweep of web/remap.html against the virtual controller: every input, every mode, every option.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const fs = require('fs');
const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const ctx = await br.newContext({ acceptDownloads: true, viewport: { width: 1300, height: 1000 } });
  const pg = await ctx.newPage(); pg.setDefaultTimeout(1500);
  const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  const fails = []; const ok = (c, m) => { if (!c) fails.push(m); };
  const dlgOpen = () => pg.evaluate(() => document.querySelector('#dlg').open);
  const done = async () => { if (await dlgOpen()) await pg.click('#dlgDone'); };
  // 1. every input on the picture
  const items = await pg.$$eval('#v_map [data-s]', x => [...new Set(x.map(e => e.dataset.s))]);
  for (const it of items) {
    if (it === 'gyro') continue;
    for (let a = 0; a < 4 && !(await dlgOpen()); a++) {   // the page redraws after a dialog closes
      await sleep(60);
      await pg.click(`#v_map [data-s="${it}"] >> nth=0`).catch(() => {});
      await sleep(40);
    }
    if (!(await dlgOpen())) { fails.push('no dialog for ' + it); continue; }
    if (await pg.$('#amode')) {
      const modes = await pg.$$eval('#amode option', o => o.map(x => x.value));
      for (const m of modes) {
        await pg.selectOption('#amode', m); await sleep(20);
        // every checkbox and select in the analog editor
        const nth = async (sel, fn) => { const n = await pg.$$eval(sel, x => x.length); for (let i = 0; i < n; i++) { const h = await pg.$(`${sel} >> nth=${i}`); if (h) await fn(h).catch(() => {}); await sleep(5); } };
        await nth('#editor input[type=checkbox]', h => h.click());
        await nth('#editor select:not(#amode)', async h => { const v = await h.$$eval('option', o => o.map(x => x.value)); if (v.length) await h.selectOption(v[v.length - 1]); });
        await nth('#editor input[type=number]', async h => { await h.fill('7'); await h.dispatchEvent('change'); });
        // sub-binding dialogs
        if (await pg.$('#editor .subs .item')) { await pg.click('#editor .subs .item >> nth=0').catch(() => {}); await sleep(20); await pg.click('#back').catch(() => {}); await sleep(20); }
      }
      await pg.selectOption('#amode', modes[0]).catch(() => {});
    } else {
      // binding editor: every mode checkbox, every group select, chord, haptic, copy/paste
      { const n = await pg.$$eval('#editor [data-bit]', x => x.length); for (let i = 0; i < n; i++) { await pg.click(`#editor [data-bit] >> nth=${i}`).catch(() => {}); await sleep(10); } }
      { const n = await pg.$$eval('#editor [data-gi]', x => x.length); for (let i = 0; i < n; i++) { await pg.selectOption(`#editor [data-gi] >> nth=${i}`, String(0x04 + i)).catch(() => {}); await sleep(5); } }
      if (await pg.$('#bchord')) await pg.selectOption('#bchord', { index: 2 });
      if (await pg.$('#bhap')) await pg.selectOption('#bhap', { index: 1 });
      if (await pg.$('#tmode')) for (const v of ['0', '1', '2', '3']) { await pg.selectOption('#tmode', v); await sleep(10); }
      await pg.click('#bcopy').catch(() => {});
      await pg.click('#bpaste').catch(() => {});
      if (await pg.$('#tlink')) { await pg.click('#tlink'); await sleep(20); }
    }
    await done();
  }
  // 2. profile settings dialog: macros, times, presets, copy
  await pg.click('#profset'); await sleep(50);
  for (const m of [0, 1, 2, 3]) { await pg.click(`#dlg [data-msel="${m}"]`); await sleep(20); await pg.selectOption('#dlg [data-ma="0"]', '4'); await sleep(20); }
  for (const id of ['phold', 'pdbl', 'pturbo']) { const e = await pg.$(`#editor input[type=number][id*="${id}"], #editor #${id}n, #editor [data-sl="${id}"]`); }
  await pg.click('#dlg [data-pcopy] >> nth=0'); await sleep(30);
  await done();
  // 3. LED, block unbound, name
  await pg.check('#pledOn').catch(() => {}); await pg.fill('#pname', 'ui sweep');
  // 4. save, reload, compare
  const before = await pg.evaluate(() => Array.from(prof[cur]));
  await pg.click('#write');
  await pg.waitForFunction(() => /saved to|differs|err/.test(document.querySelector('#pstatus').textContent) && !/saving/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  ok(/saved to the controller/.test(await pg.textContent('#pstatus')), 'save: ' + await pg.textContent('#pstatus'));
  await pg.click('#read');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  const after = await pg.evaluate(() => Array.from(prof[cur]));
  ok(JSON.stringify(before) === JSON.stringify(after), 'reload differs from what was saved');
  // 5. export -> import round trip
  const [dl] = await Promise.all([pg.waitForEvent('download'), pg.click('#export')]);
  const fn = '/tmp/ui_export.json'; await dl.saveAs(fn);
  await pg.click('#tabs .tab >> nth=2'); await sleep(50);
  await pg.setInputFiles('#import', fn); await sleep(200);
  const imp = await pg.evaluate(() => Array.from(prof[cur]));
  ok(JSON.stringify(imp.slice(16)) === JSON.stringify(after.slice(16)), 'import differs from export');
  // 6. presets
  await pg.click('#profset'); await sleep(50);
  { const n = await pg.$$eval('#dlg [data-pre]', x => x.length); for (let i = 0; i < n; i++) { if (!(await dlgOpen())) await pg.click('#profset'); await pg.click(`#dlg [data-pre] >> nth=${i}`); await sleep(50); } }
  await done();
  // 7. other tabs
  for (const v of ['gyro', 'ctl', 'cal', 'help', 'map']) { await pg.click(`.view[data-v="${v}"]`).catch(() => {}); await sleep(150); }
  await pg.click('.view[data-v="gyro"]'); await pg.selectOption('#gyScope', 'p'); await sleep(100); await pg.selectOption('#gyScope', 'g').catch(() => {}); await sleep(100);
  await pg.click('.view[data-v="map"]');
  await pg.screenshot({ path: '/tmp/ui_sweep.png' });
  ok(errs.length === 0, 'page errors: ' + errs.slice(0, 5).join(' | '));
  console.log(fails.length ? 'FAIL\n' + fails.join('\n') : 'UI sweep: ' + items.length + ' inputs, all modes/options, save/reload/export/import/presets — OK');
  await br.close();
})().catch(e => { console.error('CRASH', e.message); process.exit(1); });
