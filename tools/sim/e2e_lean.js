// Remapper: "Slow return to centre" (v51.9) in the stick editor: set, saved to the virtual controller, read back.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
let bad = 0; const check = (n, c, i = '') => { if (!c) bad++; console.log((c ? 'PASS ' : 'FAIL ') + n + (c ? '' : ' — ' + i)); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(5000);
  const r = await pg.evaluate(() => {
    const r = {}; cur = 0; prof[0].fill(0, LEAN, LEAN + 6); prof[0][aOff('ls')] = 0; render(); openSel('ls');
    r.offFirst = !!document.getElementById('lnret') && !document.getElementById('lnmul');
    const n = document.getElementById('lnret_n'); n.value = '1.5'; n.onchange();
    r.after = [prof[0][LEAN], prof[0][LEAN + 1], prof[0][LEAN + 2]];
    r.more = !!document.getElementById('lnmul') && document.getElementById('lnx').checked && !document.getElementById('lny').checked;
    const m = document.getElementById('lnmul_n'); m.value = '3'; m.onchange();
    const y = document.getElementById('lny'); y.checked = true; y.onchange({ target: y });
    r.bytes = [prof[0][LEAN], prof[0][LEAN + 1], prof[0][LEAN + 2]];
    r.row = [...document.querySelectorAll('[data-s="ls"]')].map(e => e.textContent).join(' | ');
    openSel('rs'); r.rsOwn = document.getElementById('lnret').value === '0';
    prof[0][aOff('rs')] = 3; render(); openSel('rs'); r.mouseNone = !document.getElementById('lnret');
    prof[0][aOff('rs')] = 0; prof[0][aOff('ls')] = 12; render(); openSel('ls'); r.xbox = !!document.getElementById('lnret');
    prof[0][aOff('ls')] = 0; render(); return r; });
  check('off at first, catch-up and axes appear once set', r.offFirst && r.more, JSON.stringify(r));
  check('1.5 s turns it on for X with 1.0x', r.after.join() === '30,20,1', r.after);
  check('3x, X and Y', r.bytes.join() === '30,60,3', r.bytes);
  check('shown under the picture', /slow return1\.5 s · X\+Y/.test(r.row), r.row);
  check('each stick its own, not in mouse mode, in Xbox stick mode', r.rsOwn && r.mouseNone && r.xbox, JSON.stringify(r));
  await pg.evaluate(() => { dirtyP.add(0); document.getElementById("write").click(); });
  await pg.waitForTimeout(3000);
  const back = await pg.evaluate(async () => { prof[0].fill(0, LEAN, LEAN + 6); await readAll(); return [...prof[0].slice(LEAN, LEAN + 6)]; });
  check('saved to the controller and read back', back.join() === '30,60,3,0,0,0', back);
  check('no page errors', !errs.length, errs.join('; '));
  await br.close(); console.log(bad ? `${bad} failed` : 'all passed'); process.exit(bad ? 1 : 0);
})();
