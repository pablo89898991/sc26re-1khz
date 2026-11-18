// Every editor control must land in the byte the firmware reads (struct remap_profile in remap.h).
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const sleep = ms => new Promise(r => setTimeout(r, ms));
// remap.h: an[i] at 256 + 18*i; shift[g] at 328 + 20*g, its analog at +2
const AN = { ls: 256, rs: 274, lp: 292, rp: 310 };
const SH = { ls: 328 + 20 * 2 + 2, rs: 328 + 20 * 3 + 2, lp: 328 + 20 * 4 + 2, rp: 328 + 20 * 5 + 2 };
// control -> [offset in remap_analog, kind, value typed, stored value]
const FIELDS = {
  dzi: [2, 'u16', 45.67, 4567], dzo: [4, 'u16', 5.67, 567], anti: [14, 'u16', 8.9, 890], ring: [12, 'u16', 77.7, 7770],
  press: [16, 'u16', 42.1, 4210], crv: [10, 'u16', 1.75, 175], rot: [9, 's8', -33, -33], tacc: [10, 'u16', 150, 150], tfric: [4, 'u16', 3.5, 350],
};
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } }); pg.setDefaultTimeout(1500);
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  const fails = []; let checked = 0;
  const bytes = () => pg.evaluate(() => Array.from(prof[cur]));
  const u16 = (b, o) => b[o] | (b[o + 1] << 8), s8 = (b, o) => (b[o] << 24) >> 24;
  const open = async it => { for (let a = 0; a < 4 && !(await pg.evaluate(() => document.querySelector('#dlg').open)); a++) { await sleep(60); await pg.click(`#v_map [data-s="${it}"] >> nth=0`).catch(() => {}); await sleep(40); } };
  for (const an of ['ls', 'rs', 'lp', 'rp']) {
    for (const shifted of [false, true]) {
      await open(an);
      if (shifted) {
        await pg.selectOption('#shb', { index: 3 }); await sleep(50);           // a shift button: opens the shifted layer
        await pg.click('.seg [data-l="1"]').catch(() => {}); await sleep(40);
      } else if (await pg.$('#shb')) { await pg.selectOption('#shb', '0'); await sleep(40); }
      const base = shifted ? SH[an] : AN[an];
      const modes = await pg.$$eval('#amode option', o => o.map(x => +x.value));
      for (const m of modes) {
        await pg.selectOption('#amode', String(m)); await sleep(30);
        const b0 = await bytes();
        if (b0[base] !== m) { fails.push(`${an}${shifted ? ' shifted' : ''}: mode ${m} not at offset ${base} (found ${b0[base]})`); continue; }
        for (const [id, [off, kind, typed, want]] of Object.entries(FIELDS)) {
          const n = await pg.$(`#${id}_n`); if (!n) continue;
          await n.fill(String(typed)); await n.dispatchEvent('change'); await n.dispatchEvent('input'); await sleep(10);
          const b = await bytes(); const got = kind === 'u16' ? u16(b, base + off) : s8(b, base + off);
          checked++;
          if (Math.abs(got - want) > 1) fails.push(`${an}${shifted ? ' shifted' : ''} mode ${m}: ${id}=${typed} -> offset ${off} holds ${got}, want ${want}`);
        }
        for (const [id, bit] of [['invx', 0x20], ['invy', 0x40], ['tball', 0x80], ['srep', 0x08], ['rinv', 0x02]]) {
          const c = await pg.$('#' + id); if (!c) continue;
          const before = (await bytes())[base + 1] & bit;
          await c.click(); await sleep(15);
          const after = (await bytes())[base + 1] & bit; checked++;
          if (before === after) fails.push(`${an} mode ${m}: checkbox ${id} does not change flag 0x${bit.toString(16)}`);
        }
        if (await pg.$('#ahap')) { await pg.selectOption('#ahap', '3'); checked++; if ((await bytes())[base + 8] !== 3) fails.push(`${an} mode ${m}: zone haptics not at +8`); }
        if (await pg.$('#aact')) { await pg.selectOption('#aact', '1'); checked++; if (((await bytes())[base + 1] & 0x11) !== 1) fails.push(`${an} mode ${m}: 'works while clicked' flag`); await pg.selectOption('#aact', '0'); }
      }
      await pg.click('#dlgDone').catch(() => {}); await sleep(60);
    }
  }
  // binding editor: modes, actions, haptic, chord
  await open('3'); const bo = 488 + 14 * 3;
  for (const [bit] of [[1], [2], [8], [16], [4], [32], [64]]) {
    const before = (await bytes())[bo]; await pg.click(`#editor [data-bit="${bit}"]`).catch(() => {}); await sleep(20);
    const after = (await bytes())[bo]; checked++; if (((before ^ after) & bit) === 0) fails.push(`binding mode bit ${bit} not at +0`);
  }
  // timing sliders in the binding editor (the profile's times)
  for (const [bit, id, off, v] of [[1, 'bhold', 118, 555], [2, 'bdbl', 120, 222], [32, 'bturbo', 122, 66]]) {
    if (!((await bytes())[bo] & bit)) { await pg.click(`#editor [data-bit="${bit}"]`); await sleep(30); }
    const n = await pg.$(`#${id}_n`); if (!n) { fails.push('binding editor: no ' + id + ' slider'); continue; }
    await n.fill(String(v)); await n.dispatchEvent('change'); await sleep(10); checked++;
    if (u16(await bytes(), off) !== v) fails.push(`binding editor ${id} not at ${off}`);
  }
  await pg.selectOption('#editor [data-gi="0:2"]', '9'); checked++; if ((await bytes())[bo + 1 + 2] !== 9) fails.push('binding action group 0 slot 2 not at +3');
  await pg.selectOption('#bhap', '2'); checked++; if (((await bytes())[bo + 13] & 3) !== 2) fails.push('binding haptic not at +13 bits 0-1');
  await pg.selectOption('#bchord', { index: 3 }); const ch = await pg.$eval('#bchord', e => +e.value); checked++;
  if (((await bytes())[bo + 13] >> 2) !== ch) fails.push('binding chord not at +13 bits 2-7');
  await pg.click('#dlgDone');
  // triggers
  await open('23'); const tb = await bytes();
  for (const [id, off, typed, want] of [['tsp', 18 + 2, 33.3, 3330], ['thip', 106 + 2, 120, 120], ['tra', 110 + 4, 10.5, 1050], ['tre', 112 + 4, 88.8, 8880]]) {
    if (id === 'thip') { await pg.selectOption('#tmode', '2'); await sleep(30); }
    const n = await pg.$(`#${id}_n`); if (!n) { fails.push('missing ' + id); continue; }
    await n.fill(String(typed)); await n.dispatchEvent('change'); await sleep(10); const b = await bytes(); checked++;
    if (Math.abs(u16(b, off) - want) > 1) fails.push(`trigger ${id} -> offset ${off} holds ${u16(b, off)}, want ${want}`);
  }
  await pg.selectOption('#tmode', '3'); checked++; if ((await bytes())[105] !== 3) fails.push('right trigger mode not at 105');
  await pg.click('#dlgDone');
  // profile settings
  await pg.click('#profset'); await sleep(50);
  for (const [id, off, v] of [['phold', 118, 444], ['pdbl', 120, 333], ['pturbo', 122, 77]]) {
    const n = await pg.$(`#${id}_n`); await n.fill(String(v)); await n.dispatchEvent('change'); await sleep(10); checked++;
    if (u16(await bytes(), off) !== v) fails.push(`${id} not at ${off}`);
  }
  await pg.click('#dlg [data-msel="2"]'); await sleep(30); await pg.selectOption('#dlg [data-ma="0"]', '7'); await sleep(30);
  const mt = await pg.$('#dlg [data-mt="0"]'); await mt.fill('250'); await mt.dispatchEvent('change'); await sleep(20);
  const mb = await bytes(); checked++; if (mb[128 + 2 * 32] !== 7 || mb[128 + 2 * 32 + 1] !== 25) fails.push(`macro 3 step 1 not at ${128 + 64}: ${mb[192]},${mb[193]}`);
  await pg.click('#dlgDone');
  await pg.check('#block'); checked++; if (!((await bytes())[17] & 1)) fails.push('block unbound flag not at 17');
  console.log(fails.length ? 'FAIL\n' + fails.slice(0, 30).join('\n') : `layout: ${checked} controls checked against remap.h — OK`);
  if (errs.length) console.log('page errors:', errs.slice(0, 3));
  await br.close();
})().catch(e => { console.error('CRASH', e.message); process.exit(1); });
