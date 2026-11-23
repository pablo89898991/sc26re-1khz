const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  for (const [name, setup] of [['all on', ''], ['no raw reads', 'src=steam'], ['no model', 'model=off'], ['no device', 'nodev']]) {
    const ctx = await br.newContext({ viewport: { width: 1600, height: 1000 } }); const pg = await ctx.newPage();
    await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await pg.goto('http://127.0.0.1:8766/overlay.html');
    if (setup !== 'nodev') await pg.evaluate(() => document.querySelector('#connect').click());
    await pg.waitForTimeout(1500);
    if (setup === 'src=steam') await pg.evaluate(() => { const s = document.querySelector('#src'); s.value = [...s.options].map(o => o.value).find(v => v !== 'raw'); s.dispatchEvent(new Event('change')); });
    if (setup === 'model=off') await pg.evaluate(() => { const s = document.querySelector('#modelOn'); s.checked = false; s.dispatchEvent(new Event('change')); });
    await pg.waitForTimeout(3000);
    const cdp = await ctx.newCDPSession(pg); await cdp.send('Performance.enable');
    const m = async () => Object.fromEntries((await cdp.send('Performance.getMetrics')).metrics.map(x => [x.name, x.value]));
    const a = await m(); await pg.waitForTimeout(4000); const b = await m();
    console.log(`${name}: busy ${((b.TaskDuration - a.TaskDuration) * 250).toFixed(0)} ms/s, script ${((b.ScriptDuration - a.ScriptDuration) * 250).toFixed(0)}, layout ${((b.LayoutDuration - a.LayoutDuration) * 250).toFixed(0)}, style ${((b.RecalcStyleDuration - a.RecalcStyleDuration) * 250).toFixed(0)}`);
    await ctx.close();
  }
  await br.close();
})();
