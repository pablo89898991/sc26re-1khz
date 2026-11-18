// End-to-end: web/remap.html driving the virtual controller (sim_server.py must run)
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const SRV = 'http://127.0.0.1:8765';
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: typeof b === 'string' ? b : JSON.stringify(b) }).then(r => r.json());
const get = p => fetch(SRV + p).then(r => r.json());
const sleep = ms => new Promise(r => setTimeout(r, ms));
const results = [];
const check = (name, ok, info = '') => { results.push([name, ok, info]); console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); };
const kbd = evs => evs.filter(e => e.split(' ')[1] === 'K').map(e => e.split(' ').slice(2).join(' '));
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message)); pg.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('file://' + __dirname + '/../../web/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  check('connect + read 4 profiles', true, await pg.textContent('#status'));

  // 1. bind A -> key A through the editor, save
  await pg.click('#v_map [data-s="0"]');
  await pg.selectOption('#dlg [data-gi="0:0"]', '4');
  await pg.click('#dlgDone');
  await pg.fill('#pname', 'e2e');
  await pg.click('#write');
  await pg.waitForFunction(() => /saved|differs|err/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  check('save profile (read-back compare)', /saved to the controller/.test(await pg.textContent('#pstatus')), await pg.textContent('#pstatus'));
  if (!/ON/.test(await pg.textContent('#onoff'))) { await pg.click('#onoff'); await pg.waitForFunction(() => /ON/.test(document.querySelector('#onoff').textContent), null, { timeout: 10000 }).catch(() => {}); }
  check('remapper on', /ON/.test(await pg.textContent('#onoff')));
  await post('/clear', {}); await post('/set', { A: 1 }); const tA = Date.now();
  const tLit = await pg.waitForFunction(() => document.querySelector('#v_map [data-s="0"]').classList.contains('on'), null, { timeout: 3000 }).then(() => Date.now() - tA).catch(() => -1);
  const tMon = await pg.waitForFunction(() => /Output/.test(document.querySelector('#mon').textContent) && !/nothing pressed/.test(document.querySelector('#mon').textContent), null, { timeout: 3000 }).then(() => Date.now() - tA).catch(() => -1);
  console.log('latency: highlight', tLit, 'ms, monitor', tMon, 'ms');
  let ev = await get('/events');
  check('A sends keyboard A', kbd(ev).some(k => k.startsWith('00 04')), kbd(ev).join(' | '));
  const lit = await pg.evaluate(() => document.querySelector('#v_map [data-s="0"]').classList.contains('on'));
  const mon = await pg.textContent('#mon');
  check('output monitor shows the key', /A/.test(mon) && /Output/.test(mon), mon.replace(/\s+/g, ' ').slice(0, 120));
  check('A highlighted on the picture while remapped', lit);
  await post('/set', { A: 0 }); await sleep(100);

  // 2. right trackpad radial menu, 4 slices; bind slice 2 to key 2
  await pg.click('#v_map [data-s="rp"]');
  await pg.selectOption('#amode', '10');
  await sleep(100);
  const slices = await pg.$$eval('#dlg .subs .item .k', x => x.map(e => e.textContent));
  check('radial dialog lists slices', slices.length >= 2, slices.join(','));
  await pg.evaluate(() => { const i = document.querySelector('#dlg [data-sl="asp"], #dlg input[data-k="asp"]'); });
  await pg.click('#dlg .subs .item >> nth=1');
  await pg.selectOption('#dlg [data-gi="0:0"]', '31'); // key 2
  await pg.click('#dlgDone');
  // haptic for zones
  await pg.click('#v_map [data-s="rp"]');
  if (await pg.$('#ahap')) await pg.selectOption('#ahap', '1');
  await pg.click('#dlgDone');
  await pg.click('#write');
  await pg.waitForFunction(() => /saved|differs|err/.test(document.querySelector('#pstatus').textContent) && !/saving/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  await sleep(300);
  check('save radial profile', /saved to the controller/.test(await pg.textContent('#pstatus')));
  await post('/clear', {});
  // slice index 1 with default slice count: angle = 360/n; touch there
  const n = slices.length; const a = 2 * Math.PI * 1 / n;
  await post('/set', { RP_TOUCH: 1, rpx: 0, rpy: 0 }); await sleep(60);
  await post('/set', { rpx: Math.round(28000 * Math.sin(a)), rpy: Math.round(28000 * Math.cos(a)) }); await sleep(120);
  await post('/set', { RP_TOUCH: 0, rpx: 0, rpy: 0 }); await sleep(150);
  ev = await get('/events');
  check('radial: slice 2 fires key 2 on lift', kbd(ev).some(k => k.startsWith('00 1f')), kbd(ev).join(' | '));
  check('radial: zone haptic tick', ev.some(e => e.split(' ')[1] === 'H'), ev.filter(e => / H /.test(e)).join(','));

  // 3. profile settings: macro 1 = A then B, bind B button -> run macro 1
  await pg.click('#profset');
  await pg.selectOption('#dlg [data-ma="0"]', '4'); await sleep(50);
  await pg.selectOption('#dlg [data-ma="1"]', '5'); await sleep(50);
  await pg.click('#dlgDone');
  await pg.click('#v_map [data-s="1"]');
  await pg.selectOption('#dlg [data-gi="0:0"]', String(0xC8));
  await pg.click('#dlgDone');
  await pg.click('#write');
  await pg.waitForFunction(() => /saved to|differs|err/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  await post('/clear', {}); await post('/set', { B: 1 }); await sleep(60); await post('/set', { B: 0 }); await sleep(300);
  ev = await get('/events');
  const ks = kbd(ev);
  check('macro plays A then B', ks.findIndex(k => k.startsWith('00 04')) >= 0 && ks.findIndex(k => k.startsWith('00 05')) > ks.findIndex(k => k.startsWith('00 04')), ks.join(' | '));

  // 4. reload from controller and compare
  const before = await pg.evaluate(() => Array.from(prof[0]));
  await pg.click('#read');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  const after = await pg.evaluate(() => Array.from(prof[0]));
  check('reload from controller = same profile', JSON.stringify(before) === JSON.stringify(after));

  // 5. live picture: stick + trackpad dot follow the controller
  await post('/set', { lsx: 32767, LP_TOUCH: 1, lpx: 20000, lpy: 0 }); await sleep(500);
  const dot = await pg.evaluate(() => { const e = document.getElementById('dot_ls'); const p = document.getElementById('dot_lp'); return [e && (+e.getAttribute('cx') - +e.dataset.x), p && p.getAttribute('opacity')]; });
  check('live stick dot moves / pad finger shown', dot[0] > 5 && dot[1] === '1', JSON.stringify(dot));
  await post('/set', { lsx: 0, LP_TOUCH: 0, lpx: 0 });
  await pg.click('#v_map [data-s="rp"]'); await sleep(200);
  await pg.screenshot({ path: '/tmp/e2e_radial.png' });
  await pg.click('#dlgDone');
  await pg.screenshot({ path: '/tmp/e2e_main.png' });
  // 6. gyro range per profile from the Gyro page
  await pg.click('.view[data-v="gyro"]'); await sleep(300);
  await pg.selectOption('#gyScope', 'p'); await sleep(200);
  const fsrOn = await pg.evaluate(() => !document.getElementById('g_gyro_fsr').disabled);
  check('gyro range editable for the profile', fsrOn);
  await pg.selectOption('#g_gyro_fsr', '500');
  await pg.click('#gyApply'); await sleep(100);
  await pg.click('.view[data-v="map"]'); await sleep(200);
  await pg.click('#write');
  await pg.waitForFunction(() => /saved to|differs|err/.test(document.querySelector('#pstatus').textContent) && !/saving/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  await sleep(200);
  const fsr = await get('/fsr');
  check('profile gyro range 500 active on the controller', fsr === 500, 'fsr=' + fsr);
  check('no page errors', errs.length === 0, errs.slice(0, 5).join(' | '));
  console.log(`\n${results.filter(r => r[1]).length} passed, ${results.filter(r => !r[1]).length} failed`);
  await br.close();
})().catch(e => { console.error(e); process.exit(1); });
