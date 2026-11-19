// Hardware test page (web/hwtest.html) against the virtual controller: connects, runs every case,
// the results match the simulator (hwt.py check), the profile slot and the active profile it
// borrowed are back as they were. Needs a fresh sim_server.py on 8765 and http.server 8766 in web/.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const { execFileSync } = require('child_process');
const fs = require('fs');
const SRV = 'http://127.0.0.1:8765';
const post = (p, b) => fetch(SRV + p, { method: 'POST', body: typeof b === 'string' ? b : JSON.stringify(b) }).then(r => r.json());
let bad = 0, total = 0;
const check = (name, ok, info = '') => { total++; if (!ok) bad++; console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); return ok; };
async function feature(op, path, data = []) {
  const p = Buffer.from(path), req = Buffer.alloc(64); req[0] = op; req[1] = p.length + 1 + data.length; p.copy(req, 2); Buffer.from(data).copy(req, 3 + p.length);
  return Buffer.from((await post('/feature', req.toString('hex'))).rsp, 'hex');
}
async function slot3() { let out = ''; for (let o = 0; o < 3232; ) { const r = await feature(0xED, 'sc26re/rmp/3/' + o.toString(16)); const d = r.slice(4, 3 + r[1]); out += d.toString('hex'); o += d.length; } return out; }
(async () => {
  /* something in slot 4 and profile 2 active before the test */
  const mine = Buffer.alloc(3232); mine[0] = 0xA7; mine[1] = 8; mine.write('mine', 2);
  for (let o = 0; o < 3232; o += 40) await feature(0xEE, 'sc26re/rmp/3/' + o.toString(16), [...mine.slice(o, o + 40)]);
  await feature(0xEF, 'sc26re/rmp/3');
  await feature(0xEE, 'sc26re/remap_profile', [1, 0]); await feature(0xEF, 'sc26re/remap_profile');
  /* the virtual controller always has its Xbox pad: say so, as the real one does with pad_out */
  await feature(0xEE, 'sc26re/pad_out', [2, 0]); await feature(0xEF, 'sc26re/pad_out');
  const before = await slot3();
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const ctx = await br.newContext({ acceptDownloads: true });
  const pg = await ctx.newPage();
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/hwtest.html');
  await pg.click('#connect');
  check('connects and finds the test firmware', await pg.waitForFunction(() => !document.querySelector('#run').disabled, null, { timeout: 15000 }).then(() => true, () => false), await pg.textContent('#status'));
  await pg.click('#run');
  const done = await pg.waitForFunction(() => /are back|RESTORE FAILED/.test(document.querySelector('#log').textContent), null, { timeout: 600000 }).then(() => true, () => false);
  check('runs every case and restores', done && /are back/.test(await pg.textContent('#log')), (await pg.textContent('#log')).slice(-300));
  const res = await pg.evaluate(() => window.hwtResults);
  check('results for every case, no errors', res && res.results.length > 20 && res.results.every(r => !r.error), res && JSON.stringify(res.results.filter(r => r.error)).slice(0, 300));
  fs.writeFileSync('/tmp/hwt_res.json', JSON.stringify(res));
  let out = ''; try { out = execFileSync('python3', [__dirname + '/hwt.py', 'check', '/tmp/hwt_res.json']).toString(); } catch (e) { out = e.stdout.toString(); }
  check('the virtual controller matches the simulator', /^0 of \d+ differ/m.test(out), out.split('\n').filter(l => !l.startsWith('ok')).join('\n').slice(0, 1500));
  check('slot 4 is back as it was', (await slot3()) === before);
  const ap = await feature(0xED, 'sc26re/remap_profile');
  check('the active profile is back', ap[3] === 1, ap.slice(0, 8).toString('hex'));
  check('no page errors', errs.length === 0, errs.join('; '));
  console.log(`${total} checks, ${bad} failed`);
  await br.close(); process.exit(bad ? 1 : 0);
})();
