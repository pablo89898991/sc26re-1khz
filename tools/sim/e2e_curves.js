// Remapper: custom 4-point response curves (firmware v51.7+) on the stick editor and the gyro page,
// saved to the virtual controller and read back. Needs sim_server.py (8765) and web on 8766.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  await pg.evaluate(() => document.querySelector('#connect').click());
  await pg.waitForTimeout(5000);
  const out = {};
  out.fw = await pg.evaluate(() => fwCrv);
  // ---- stick: left stick to Steam, custom curve on, point 2 typed, saved
  out.stick = await pg.evaluate(async () => {
    const r = {}; cur = 0; render(); openSel('ls');
    const o = aOff('ls'); prof[cur][o] = 0; render();
    const m = document.getElementById('crvmode'); r.modeRow = !!m;
    m.value = '1'; m.onchange({ target: m });
    r.on = crvOn(0, 0); r.editor = document.querySelectorAll('#crvS circle').length;
    r.sliderGone = !document.getElementById('crv');
    const x = document.getElementById('crvS_x1'), y = document.getElementById('crvS_y1');
    x.value = '16384'; x.onchange({ target: x }); y.value = '4000'; y.onchange({ target: y });
    r.pts = crvPts(0, 0); r.dirty = dirtyP.has(0);
    return r; });
  // drag point 4 to the middle of the picture with real pointer events
  const box = await pg.locator('#crvS_svg').boundingBox();
  const c = await pg.locator('#crvS_svg circle[data-i="3"]').boundingBox();
  await pg.mouse.move(c.x + c.width / 2, c.y + c.height / 2); await pg.mouse.down();
  await pg.mouse.move(box.x + box.width / 2, box.y + box.height / 2, { steps: 5 }); await pg.mouse.up();
  out.drag = await pg.evaluate(() => crvPts(0, 0).slice(6));
  await pg.evaluate(() => document.querySelector('#dlg').close && document.querySelector('#dlg').close());
  await pg.evaluate(() => document.querySelector('#write').click());
  await pg.waitForTimeout(6000);
  out.saved = await pg.evaluate(async () => {
    const back = await crvRead(0);
    return { status: document.getElementById('pstatus').textContent, same: !!back && back.every((v, i) => v === crv[0][i]), on: back && back[0] }; });
  // ---- gyro page: global mouse curve on, range 500, Save; read back from the controller
  out.gyro = await pg.evaluate(async () => {
    showView('gyro'); const r = {}; $('#gyScope').value = 'g'; gyScopeShow();
    const s = document.getElementById('gmcm'); s.value = '1'; s.onchange({ target: s });
    r.editor = document.querySelectorAll('#crvGM circle').length;
    const mx = document.getElementById('gmcmax'); mx.value = '500'; mx.onchange({ target: mx });
    r.dimmed = document.getElementById('g_gm_curve').closest('.row').classList.contains('dim');
    await new Promise(res => setTimeout(res, 300));
    await document.getElementById('gySave').onclick();
    const back = await crvRead(CRV_G); r.back = back && [back[0], new DataView(back.buffer).getInt16(2, true)];
    // profile scope: the profile's own slot, global one untouched
    $('#gyScope').value = 'p'; gyScopeShow();
    r.profileSlotOff = document.getElementById('gmcm').value === '0';
    $('#gyScope').value = 'g'; gyScopeShow();
    r.globalStillOn = document.getElementById('gmcm').value === '1';
    return r; });
  // ---- reload the page: everything comes back from the controller
  await pg.reload(); await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(5000);
  out.reload = await pg.evaluate(() => ({ ls: crvOn(0, 0), pts: crvPts(0, 0), gm: crvOn(CRV_G, 2), max: crvMax(CRV_G) }));
  const ok = out.fw === true && out.stick.modeRow && out.stick.on && out.stick.editor === 4 && out.stick.sliderGone &&
    out.stick.pts[2] === 16384 && out.stick.pts[3] === 4000 && out.stick.dirty &&
    Math.abs(out.drag[0] - 16384) < 1200 && Math.abs(out.drag[1] - 16384) < 1200 &&
    out.saved.same && out.saved.on === 1 && /saved/.test(out.saved.status) &&
    out.gyro.editor === 4 && out.gyro.dimmed && out.gyro.back && out.gyro.back[0] === 4 && out.gyro.back[1] === 500 &&
    out.gyro.profileSlotOff && out.gyro.globalStillOn &&
    out.reload.ls && out.reload.pts[2] === 16384 && out.reload.gm && out.reload.max === 500 && !errs.length;
  console.log((ok ? 'PASS ' : 'FAIL ') + JSON.stringify(out) + (errs.length ? ' errors: ' + errs.join('; ') : ''));
  await br.close(); process.exit(ok ? 0 : 1);
})();
