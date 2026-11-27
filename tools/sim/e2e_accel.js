// Remapper: the accelerometer (firmware v51.8+), its own entry under the picture: edited, saved and read back.
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
    const r = {}; cur = 0; acc[0].fill(0); render(); // a run before may have saved it
    const entry = document.querySelector('#colB [data-s="acc"]'); r.entry = entry && entry.textContent; r.entryNothing = /nothing/.test(r.entry);
    openSel('gyp'); r.gypLink = !!document.getElementById('toAcc') && !document.getElementById('acst');
    document.getElementById('toAcc').click(); r.viaLink = sel;
    openSel('gyp'); document.querySelector('#colB [data-s="acc"]').click(); r.viaEntry = sel;
    const st = document.getElementById('acst'); r.row = !!st; r.noSensYet = !document.getElementById('acsens');
    st.value = '2'; st.onchange({ target: st });
    r.defaults = [acc[0][0], acc[0][1], accDV(0).getUint16(2, true)];
    const se = document.getElementById('acsens'); se.value = '150'; se.onchange({ target: se });
    r.note = document.getElementById('acsensN').textContent;
    const dz = document.getElementById('acdz'); dz.value = '12.5'; dz.onchange({ target: dz });
    const inv = document.querySelector('.acf[data-f="1"]'); inv.checked = true; inv.onchange();
    let gy = document.getElementById('acgy'); r.gyDefault = gy.value;
    gy.value = '192'; gy.onchange({ target: gy }); r.gy192 = acc[0][1]; gy = document.getElementById('acgy'); r.gyShown = gy.value;
    gy.value = '0'; gy.onchange({ target: gy });
    const bt = document.getElementById('acbt'); bt.value = '2'; bt.onchange({ target: bt });
    const bs = document.getElementById('acbs'); r.bsDefault = bs.value; bs.value = '150'; bs.onchange({ target: bs });
    r.bsNote = document.getElementById('acbsN').textContent;
    r.subs = [...document.querySelectorAll('#editor [data-sub]')].map(e => e.dataset.sub).join(',');
    r.under = [...document.querySelectorAll('#colB [data-s]')].map(e => e.dataset.s).join(',');
    document.querySelector('#editor [data-sub="66"]').click(); r.opened = sel; r.back = (document.getElementById('back') || {}).textContent;
    document.getElementById('back').click(); r.backTo = sel;
    const gsel = document.querySelector('[data-ga="0:0"]'); r.noProfileOpts = ![...gsel.options].some(o => +o.value >= 0xC0 && +o.value < 0xD0);
    gsel.value = '26'; gsel.onchange();
    const sh0 = document.querySelector('[data-ga="6:0"]'), sh1 = document.querySelector('[data-ga="6:1"]'); sh0.value = '225'; sh0.onchange(); sh1.value = '27'; sh1.onchange();
    const gs = document.getElementById('acgs'); r.gsDefault = gs.value; gs.value = '200'; gs.onchange({ target: gs });
    r.gsNote = document.getElementById('acgsN').textContent;
    r.bytes = Array.from(acc[0].slice(0, 26)); r.dirty = dirtyP.has(0);
    r.summary = document.querySelector('#colB [data-s="acc"]').textContent;
    r.gestRows = [...document.querySelectorAll('#colB [data-s="acc"]')].map(e => e.textContent).slice(1);
    return r; });
  await pg.evaluate(() => document.querySelector('#write').click());
  await pg.waitForTimeout(6000);
  out.saved = await pg.evaluate(async () => { const b = await accRead(0); return { status: document.getElementById('pstatus').textContent, b: b && Array.from(b.slice(0, 26)) }; });
  await pg.reload(); await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(5000);
  out.reload = await pg.evaluate(() => { cur = 0; render(); openSel('acc'); return { g: document.querySelector('[data-ga="6:1"]').value, b: Array.from(acc[0].slice(0, 26)), st: document.getElementById('acst').value, sens: document.getElementById('acsens').value, bt: document.getElementById('acbt').value }; });
  const want = [2, 1, 150, 0, 0xe2, 0x04, 2, 0, 150, 0, 200, 0, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 225, 27];
  const e = out.edit;
  const ui = e.entryNothing && e.gypLink && e.viaLink === 'acc' && e.viaEntry === 'acc' && /^← Accelerometer$/.test(e.back) && e.backTo === 'acc' &&
    /66,67,68,69/.test(e.under) && /tilt → right stick · tilt presses 8-way · 2 gestures/.test(e.summary) && e.gestRows.length === 2 && /Shake/.test(e.gestRows[1]);
  const ok = ui && out.fw === true && out.edit.row && out.edit.noSensYet && out.edit.defaults[0] === 2 && out.edit.defaults[1] === 128 && out.edit.defaults[2] === 100 && out.edit.gyDefault === '128' && out.edit.gy192 === 193 && out.edit.gyShown === '192' &&
    /30\.0°/.test(out.edit.note) && out.edit.bsDefault === '100' && out.edit.noProfileOpts && out.edit.gsDefault === '100' && /0\.75 g/.test(out.edit.gsNote) && out.reload.g === '27' && /13\.3°/.test(out.edit.bsNote) && /66,67,68,69/.test(out.edit.subs) && String(out.edit.opened) === '66' && out.reload.bt === '2' && JSON.stringify(out.edit.bytes) === JSON.stringify(want) && out.edit.dirty &&
    /saved/.test(out.saved.status) && JSON.stringify(out.saved.b) === JSON.stringify(want) &&
    JSON.stringify(out.reload.b) === JSON.stringify(want) && out.reload.st === '2' && out.reload.sens === '150' && !errs.length;
  console.log((ok ? 'PASS ' : 'FAIL ') + JSON.stringify(out) + (errs.length ? ' errors: ' + errs.join('; ') : ''));
  await br.close(); process.exit(ok ? 0 : 1);
})();
