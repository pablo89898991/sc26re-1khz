// End-to-end: Steam Input .vdf import / export in web/remap.html against the virtual controller (sim_server.py must run)
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const V = require('../../web/vdf.js'), fs = require('fs');
const FILE = process.argv[2];
const SRV = 'http://127.0.0.1:8765';
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: JSON.stringify(b) }).then(r => r.json());
const get = p => fetch(SRV + p).then(r => r.json());
const sleep = ms => new Promise(r => setTimeout(r, ms));
let bad = 0; const check = (n, ok, i = '') => { if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + n + (i ? ' — ' + i : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 }, acceptDownloads: true });
  const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  await pg.setInputFiles('#vimport', FILE);
  await pg.waitForSelector('#vdlg[open]', { timeout: 5000 });
  const sel = await pg.$$eval('#vbody select[id^=vs]', x => x.map(e => e.selectedOptions[0].textContent));
  check('dialog: 4 slots with the default choice', sel.length === 4 && /In-Game/.test(sel[0]) && /layer/.test(sel[1]), sel.join(' | '));
  const nGame = await pg.$$eval('#vgame select', x => x.length);
  check('game action table', nGame > 50, String(nGame));
  const esc = await pg.$eval('#vgame select[data-a=OpenMainMenu]', e => e.value);
  check('manifest default key (Esc) preselected', esc === String(0x29), esc);
  await pg.selectOption('#vgame select[data-a=Attack]', String(0x70));
  await pg.selectOption('#vgame select[data-a=Reload]', String(0x15));
  const notes = await pg.$$eval('#vnotes li', x => x.map(e => e.textContent));
  check('notes listed', notes.length > 2, notes.length + ' notes');
  await pg.selectOption('#vs3', '');                      // keep profile 4
  const before4 = await pg.evaluate(() => Array.from(prof[3]));
  await pg.click('#vsave');
  await pg.waitForFunction(() => /imported and saved|differs|err/.test(document.querySelector('#pstatus').innerHTML), null, { timeout: 120000 });
  const st = await pg.textContent('#pstatus');
  check('load + save all to the controller (read back equal)', /imported and saved: profile 1, 2, 3/.test(st), st);
  // what the page saved = what vdf.js makes with the same choices
  const im = new V.Importer(V.parse(fs.readFileSync(FILE, 'utf8')), { gameMap: Object.assign(new V.Importer(V.parse(fs.readFileSync(FILE, 'utf8'))).gameDefaults(), { Attack: 0x70, Reload: 0x15 }), slots: [0, 4, 1, -1] });
  const ref = im.run();
  const page = await pg.evaluate(() => prof.map(b => Array.from(b)));
  check('profiles 1-3 as imported', [0, 1, 2].every(k => ref.profiles[k].every((v, i) => v === page[k][i])));
  check('profile 4 kept', before4.every((v, i) => v === page[3][i]));
  // Attack is on the right trigger in that layout: pull it, the mouse left button goes down
  await pg.click('[data-p="0"]').catch(() => {});
  if (!/ON/.test(await pg.textContent('#onoff'))) { await pg.click('#onoff'); await sleep(300); }
  await post('/clear', {}); await post('/set', { tr: 32767, RT_CLICK: 1 }); await sleep(250);
  const ev = await get('/events'); await post('/set', { tr: 0, RT_CLICK: 0 });
  check('Attack (right trigger) -> mouse left', ev.some(e => e.split(' ')[1] === 'M' && (+e.split(' ')[2] & 1)), ev.filter(e => / M /.test(e)).slice(0, 3).join(' | '));
  // export
  const [dl] = await Promise.all([pg.waitForEvent('download'), pg.click('#vexport')]);
  const txt = fs.readFileSync(await dl.path(), 'utf8');
  const back = new V.Importer(V.parse(txt), { slots: [0, 1, 2, 3] }).run();
  check('export parses and has 4 sets/layers', back.list.length === 4, back.list.map(x => x.title + (x.layer ? '(L)' : '')).join(', '));
  const d = [0, 1, 2, 3].map(k => page[k].reduce((n, v, i) => n + (v !== back.profiles[k][i] && !(i >= 2 && i < 16 && !page[k][2])), 0));
  check('export -> import gives the same 4 profiles', d.every(x => x === 0), d.join(',') + ' ' + JSON.stringify([0,1,2,3].map(k => page[k].map((v, i) => v !== back.profiles[k][i] ? [i, v, back.profiles[k][i]] : null).filter(x => x))));
  fs.writeFileSync('/tmp/p4.json', JSON.stringify(page[3]));
  check('no page errors', !errs.length, errs.join(' | '));
  await br.close(); process.exit(bad ? 1 : 0);
})().catch(e => { console.log('FAIL crash', e); process.exit(1); });
