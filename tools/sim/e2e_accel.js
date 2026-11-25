// Remapper: accelerometer tilt -> stick (firmware v51.8+) in the profile's gyro editor, saved and read back.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  await pg.evaluate(() => document.querySelector('#connect').click());
  await pg.waitForTimeout(5000);
  const out = {};
  out.fw = await pg.evaluate(() => fwAcc);
  out.edit = await pg.evaluate(() => {
    const r = {}; cur = 0; render(); openSel('gyp');
    const st = document.getElementById('acst'); r.row = !!st; r.noSensYet = !document.getElementById('acsens');
    st.value = '2'; st.onchange({ target: st });
    r.defaults = [acc[0][0], acc[0][1], accDV(0).getUint16(2, true)];
    const se = document.getElementById('acsens'); se.value = '150'; se.onchange({ target: se });
    r.note = document.getElementById('acsensN').textContent;
    const dz = document.getElementById('acdz'); dz.value = '12.5'; dz.onchange({ target: dz });
    const inv = document.querySelector('.acf[data-f="1"]'); inv.checked = true; inv.onchange();
    const pause = document.querySelector('.acf[data-f="128"]'); pause.checked = false; pause.onchange();
    r.bytes = Array.from(acc[0].slice(0, 6)); r.dirty = dirtyP.has(0);
    return r; });
  await pg.evaluate(() => document.querySelector('#write').click());
  await pg.waitForTimeout(6000);
  out.saved = await pg.evaluate(async () => { const b = await accRead(0); return { status: document.getElementById('pstatus').textContent, b: b && Array.from(b.slice(0, 6)) }; });
  await pg.reload(); await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(5000);
  out.reload = await pg.evaluate(() => { cur = 0; render(); openSel('gyp'); return { b: Array.from(acc[0].slice(0, 6)), st: document.getElementById('acst').value, sens: document.getElementById('acsens').value }; });
  const want = [2, 1, 150, 0, 0xe2, 0x04];
  const ok = out.fw === true && out.edit.row && out.edit.noSensYet && out.edit.defaults[0] === 2 && out.edit.defaults[1] === 128 && out.edit.defaults[2] === 100 &&
    /30\.0°/.test(out.edit.note) && JSON.stringify(out.edit.bytes) === JSON.stringify(want) && out.edit.dirty &&
    /saved/.test(out.saved.status) && JSON.stringify(out.saved.b) === JSON.stringify(want) &&
    JSON.stringify(out.reload.b) === JSON.stringify(want) && out.reload.st === '2' && out.reload.sens === '150' && !errs.length;
  console.log((ok ? 'PASS ' : 'FAIL ') + JSON.stringify(out) + (errs.length ? ' errors: ' + errs.join('; ') : ''));
  await br.close(); process.exit(ok ? 0 : 1);
})();
