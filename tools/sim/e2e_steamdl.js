// Remapper: "Download Steam Input" (a steam://controllerconfig/<game>/<id> link or the number). Inside SC2 Overlay
// through window.sc26reApp.steamConfig; in the browser through the app on 127.0.0.1:27150 (a stand-in here).
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
const http = require('http'), fs = require('fs');
const VDF = fs.readFileSync('/mnt/user-data/uploads/0289a2b4-3737914488_controller_config.vdf', 'utf8');
let bad = 0; const check = (n, c, i = '') => { if (!c) bad++; console.log((c ? 'PASS ' : 'FAIL ') + n + (c ? '' : ' — ' + i)); };
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox'] });
  const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
  await pg.goto('http://127.0.0.1:8766/remap.html');
  await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(5000);
  const run = async (link) => pg.evaluate(async (link) => { const d = document.getElementById('vdlg'); if (d.open) d.close();
    document.getElementById('vdlid').value = link; document.getElementById('vdl').click();
    for (let i = 0; i < 50 && !d.open && !/err/.test(document.getElementById('pstatus').innerHTML); i++) await new Promise(r => setTimeout(r, 100));
    return { open: d.open, head: d.open ? d.querySelector('p.note b')?.textContent : '', status: document.getElementById('pstatus').textContent }; }, link);
  // 1. no app running: a clear message
  let r = await run('steam://controllerconfig/1286680/3843516937');
  check('no app: says SC2 Overlay is needed', !r.open && /newest SC2 Overlay/.test(r.status), r.status);
  // 2. not a link
  r = await run('hello'); check('not a link: says what to paste', !r.open && /steam:\/\/controllerconfig/.test(r.status), r.status);
  // 3. the app on 127.0.0.1:27150 (stand-in): the id from the link, the import dialog opens
  let asked = null;
  const srv = http.createServer((q, s) => { const u = new URL(q.url, 'http://x'); asked = u.searchParams.get('id');
    s.writeHead(200, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' }); s.end(JSON.stringify({ id: asked, title: 'Deadlock test', filename: asked + '_controller_config.vdf', vdf: VDF })); });
  await new Promise(res => srv.listen(27150, '127.0.0.1', res));
  r = await run('steam://controllerconfig/1286680/3843516937');
  check('browser + app: downloads by the link\'s number and opens the import', r.open && asked === '3843516937' && /downloaded “Deadlock test”/.test(r.status), JSON.stringify(r) + ' asked ' + asked);
  r = await run('3737914488'); check('a bare number works too', r.open && asked === '3737914488', JSON.stringify(r));
  r = await run('https://steamcommunity.com/sharedfiles/filedetails/?id=3725332313'); check('a community page link works too', r.open && asked === '3725332313', JSON.stringify(r));
  srv.close();
  // 4. inside the app: window.sc26reApp.steamConfig
  await pg.evaluate(v => { window.sc26reApp = { steamConfig: async id => ({ id, title: 'in app ' + id, filename: '', vdf: v }) }; }, VDF);
  r = await run('steam://controllerconfig/1286680/3843516937'); check('inside the app: through window.sc26reApp', r.open && /in app 3843516937/.test(r.status), JSON.stringify(r));
  await pg.evaluate(() => { window.sc26reApp = { steamConfig: async () => { throw new Error('Steam has no layout 1 (or it is private)'); } }; });
  r = await run('1234567'); check('an error from Steam is shown', !r.open && /private/.test(r.status), r.status);
  // 5. the file import still works the same
  const fi = await pg.evaluate(async v => { const d = document.getElementById('vdlg'); if (d.open) d.close(); await vdfImport(v, 'x.vdf'); return d.open; }, VDF);
  check('file import path unchanged', fi);
  check('no page errors', !errs.length, errs.join('; '));
  await br.close(); console.log(bad ? `${bad} failed` : 'all passed'); process.exit(bad ? 1 : 0);
})();
