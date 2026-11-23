// SC2 Overlay 1.4 two windows: ?part=model draws the 3D controller and reports its area, reads no menus;
// ?part=menu draws no 3D, reads the menus and asks to be shown only while a menu is up.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  let fail = 0; const say = (ok, t) => { if (!ok) fail++; console.log((ok ? 'PASS ' : 'FAIL ') + t); };
  const open = async part => { const pg = await br.newPage({ viewport: { width: part === 'model' ? 480 : 1600, height: part === 'model' ? 300 : 900 } }); pg.errs = []; pg.on('pageerror', e => pg.errs.push(e.message));
    await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await pg.addInitScript(() => { window.__ipc = []; window.sc26reApp = { edit: v => __ipc.push(['edit', v]), arrange() {}, firmware: v => __ipc.push(['fw', v]), battery: v => __ipc.push(['bat', v]), modelRect: v => __ipc.push(['rect', v]), menuShow: v => __ipc.push(['menu', v]) }; });
    await pg.goto('http://127.0.0.1:8766/overlay.html?pip=1&app=1&part=' + part + '#debug'); await pg.evaluate(() => document.querySelector('#connect').click()); await pg.waitForTimeout(5000); return pg; };
  const model = await open('model');
  const mi = await model.evaluate(() => ({ ipc: __ipc.map(x => x[0]), rect: (__ipc.find(x => x[0] === 'rect') || [])[1], view: getComputedStyle(document.querySelector('#view')).width, vis: document.querySelector('#view').style.visibility, ready: window.__ov && __ov.ready }));
  say(mi.rect && mi.rect.size === 30 && mi.rect.x === 85 && mi.view === '480px' && mi.vis === '' && !mi.ipc.includes('menu') && !model.errs.length, 'model window: area sent, model fills the window, no menu ' + JSON.stringify(mi) + model.errs.join(';'));
  say(mi.ipc.includes('bat') && mi.ipc.includes('fw'), 'model window reads battery and firmware');
  const menu = await open('menu');
  const before = await menu.evaluate(() => ({ ipc: __ipc.slice(), vis: document.querySelector('#view').style.visibility }));
  say(before.vis === 'hidden' && !before.ipc.some(x => x[0] === 'rect' || x[0] === 'bat' || x[0] === 'fw') && before.ipc.filter(x => x[0] === 'menu').every(x => x[1] === false), 'menu window: no 3D, no battery/firmware, hidden at rest ' + JSON.stringify(before));
  await menu.evaluate(() => { __ipc.length = 0; __menu.set([0, 0, 0, 1, 0x11, 8, 2, 0, 0,0,0,0,0,0,0,0,0,0, 1,2,3,4,5,6,7,8,9,10, ...new Array(24).fill(0)]); });
  await menu.waitForTimeout(200);
  const on = await menu.evaluate(() => __ipc.filter(x => x[0] === 'menu').map(x => x[1]));
  say(on.includes(true), 'menu window asks to be shown while a menu is up ' + JSON.stringify(on));
  await menu.evaluate(() => { __ipc.length = 0; window.__ovEdit(true); }); await menu.waitForTimeout(100);
  const ed = await menu.evaluate(() => __ipc.slice());
  await menu.evaluate(() => window.__ovEdit(false));
  say(ed.some(x => x[0] === 'edit' && x[1] === true), 'arranging from the menu window ' + JSON.stringify(ed));
  await br.close(); process.exit(fail ? 1 : 0);
})();
