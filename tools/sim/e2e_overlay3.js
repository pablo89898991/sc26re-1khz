// Remapper Overlay tab (v51 test 84): the overlay inside the remapper reads sc26re/osd from the
// virtual controller and draws the radial menu with the slice names; slice names typed in the tab
// reach it; profile change note. Needs sim_server.py (8765) and the web folder served on 8766.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const SRV = 'http://127.0.0.1:8765';
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: JSON.stringify(b) }).then(r => r.json());
const sleep = ms => new Promise(r => setTimeout(r, ms));
let bad = 0, total = 0;
const check = (name, ok, info = '') => { total++; if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  const pg = await br.newPage({ viewport: { width: 1300, height: 1000 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  pg.on('dialog', d => d.accept());
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.addInitScript(() => { try { localStorage.removeItem('sc26re_menu_labels'); } catch (e) {} });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  await pg.click('#connect');
  await pg.waitForFunction(() => /loaded/.test(document.querySelector('#pstatus').textContent), null, { timeout: 30000 });
  // profile 1: right trackpad radial menu, 4 slices: up W, gate TR (slice 2) E, right D, gate BR Space
  await pg.evaluate(() => { const b = prof[0]; b.fill(0, 256, 256 + 4 * 18); b[256 + 3 * 18] = 10; b[256 + 3 * 18 + 6] = 4; b[256 + 3 * 18 + 2] = 0xC4; b[256 + 3 * 18 + 3] = 9;
    for (const [s, k] of [[42, 26], [51, 8], [45, 7], [53, 44]]) b[488 + s * 14 + 1] = k; cur = 0; render(); });
  await pg.fill('#pname', 'menus');
  await pg.click('#write');
  await pg.waitForFunction(() => /saved|differs|err/.test(document.querySelector('#pstatus').textContent), null, { timeout: 60000 });
  check('profile saved', /saved to the controller/.test(await pg.textContent('#pstatus')), await pg.textContent('#pstatus'));
  await pg.selectOption('#activeProf', '0');
  if (!/ON/.test(await pg.textContent('#onoff'))) { await pg.click('#onoff'); await sleep(500); }
  await pg.click('.view[data-v="ovl"]');
  const rows = await pg.$$eval('#ovLabels .ovl', l => l.map(e => e.placeholder));
  check('slice name rows: 4 slices with their actions', JSON.stringify(rows) === JSON.stringify(['W', 'E', 'D', 'Space']), JSON.stringify(rows));
  await pg.fill('#ovLabels .ovl[data-k="0:45"]', 'Grenade');
  const fr = await (await pg.waitForSelector('#ovBox iframe')).contentFrame();
  await fr.waitForFunction(() => window.__menu && window.__menu.ok === true, null, { timeout: 20000 }).catch(() => {});
  check('overlay inside the tab reads the menu state', await fr.evaluate(() => window.__menu && window.__menu.ok), String(await fr.evaluate(() => window.__menu && window.__menu.ok)));
  await post('/set', { RP_TOUCH: 1, rpx: 28000, rpy: 0 });
  const st = await fr.waitForFunction(() => { const s = window.__menu.st; return s && s.menus.length ? s : null; }, null, { timeout: 5000 }).then(h => h.jsonValue()).catch(() => null);
  check('touching: right pad radial open, right slice (2nd of 4) chosen', st && st.menus[0].inp === 1 && st.menus[0].sel === 1 && st.menus[0].n === 4, JSON.stringify(st));
  await sleep(200);
  const px = await fr.evaluate(() => { const c = document.querySelector('#menu'), d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 3; i < d.length; i += 4 * 7) if (d[i]) n++; return n; });
  check('menu drawn on the overlay', px > 500, px + ' px');
  await pg.screenshot({ path: '/tmp/shots/ovtab.png' });
  await post('/set', { RP_TOUCH: 0 });
  await fr.waitForFunction(() => window.__menu.st && !window.__menu.st.menus.length, null, { timeout: 5000 }).catch(() => {});
  check('lift: menu closed', await fr.evaluate(() => !window.__menu.st.menus.length));
  // profile change: the note shows the new profile's name
  const pick = v => pg.evaluate(v => { const e = document.querySelector('#activeProf'); e.value = v; e.dispatchEvent(new Event('change')); }, v);
  await pick('1');
  const t = await fr.waitForFunction(() => document.querySelector('#toast').classList.contains('on') ? document.querySelector('#toast').textContent : null, null, { timeout: 5000 }).then(h => h.jsonValue()).catch(() => '');
  check('profile change note', /^Profile 2/.test(t), t);
  await pick('0');
  check('no page errors', !errs.length, errs.join(' | '));
  console.log(`overlay tab: ${total - bad}/${total}`);
  await br.close(); process.exit(bad ? 1 : 0);
})();
