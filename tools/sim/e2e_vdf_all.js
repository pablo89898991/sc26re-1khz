// every .vdf given: the page imports it (dialog, load + save to the virtual controller), shows the notes, exports again; no page errors
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
let bad = 0; const check = (n, ok, i = '') => { if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + n + (i ? ' — ' + i : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  for (const f of process.argv.slice(2)) {
    const pg = await br.newPage({ viewport: { width: 1300, height: 1000 }, acceptDownloads: true });
    const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
    pg.on('dialog', d => d.accept());
    await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await pg.goto('file://' + __dirname + '/../../web/remap.html');
    await pg.click('#connect');
    await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
    await pg.setInputFiles('#vimport', f);
    await pg.waitForSelector('#vdlg[open]', { timeout: 5000 });
    const notes = await pg.$$eval('#vnotes li', x => x.map(e => e.textContent));
    await pg.click('#vsave');
    await pg.waitForFunction(() => /imported and saved|differs|err/.test(document.querySelector('#pstatus').innerHTML), null, { timeout: 120000 });
    const st = await pg.textContent('#pstatus');
    const name = f.split('/').pop();
    check(name + ': import + save', /imported and saved/.test(st), st);
    const [dl] = await Promise.all([pg.waitForEvent('download'), pg.click('#vexport')]);
    check(name + ': export', !!(await dl.path()));
    check(name + ': no page errors', !errs.length, errs.join(' | '));
    console.log('   notes: ' + notes.join(' / ').slice(0, 400));
    await pg.close();
  }
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e); process.exit(1); });
