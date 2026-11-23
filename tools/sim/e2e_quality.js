// Overlay quality "light": the choice survives the reload it triggers and turns MSAA off.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  const pg = await br.newPage(); const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.goto('http://127.0.0.1:8766/overlay.html'); await pg.waitForTimeout(1500);
  const aa = () => pg.evaluate(() => { const c = document.querySelector('#view canvas'); const gl = c.getContext('webgl2') || c.getContext('webgl'); return gl.getContextAttributes().antialias; });
  const before = await aa();
  await pg.evaluate(() => { const s = document.querySelector('#quality'); s.value = 'light'; s.dispatchEvent(new Event('input', { bubbles: true })); s.dispatchEvent(new Event('change', { bubbles: true })); });
  await pg.waitForTimeout(2500);
  const after = await aa(), val = await pg.evaluate(() => document.querySelector('#quality').value);
  const ok = before === true && after === false && val === 'light' && !errs.length;
  console.log((ok ? 'PASS' : 'FAIL') + ` quality light: AA ${before} -> ${after}, setting after reload ${val} ${errs.join('; ')}`);
  await br.close(); process.exit(ok ? 0 : 1);
})();
