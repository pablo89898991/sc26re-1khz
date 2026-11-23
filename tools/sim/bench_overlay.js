// CPU use of the 3D overlay (embedded in the remapper's Overlay tab and as its own page), 5 s idle.
// Needs sim_server.py and web on 8766. Prints main-thread busy ms per second (all frames) and fps.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
(async () => {
  const br = await chromium.launch({ executablePath: '/usr/bin/google-chrome', args: ['--no-sandbox', '--use-gl=swiftshader', '--enable-unsafe-swiftshader'] });
  for (const [name, url, tab] of [['remapper Overlay tab', 'remap.html', true], ['overlay page', 'overlay.html', false]]) {
    const ctx = await br.newContext({ viewport: { width: 1600, height: 1000 } });
    const pg = await ctx.newPage();
    await pg.addInitScript({ path: __dirname + '/webhid_shim.js' });
    await pg.goto('http://127.0.0.1:8766/' + url);
    await pg.evaluate(() => document.querySelector('#connect').click());
    await pg.waitForTimeout(1500);
    if (tab) await pg.evaluate(() => { const b = [...document.querySelectorAll('button')].find(x => x.textContent.trim() === 'Overlay'); b && b.click(); });
    await pg.waitForTimeout(6000);
    const cdp = await ctx.newCDPSession(pg); await cdp.send('Performance.enable');
    const m = async () => Object.fromEntries((await cdp.send('Performance.getMetrics')).metrics.map(x => [x.name, x.value]));
    const a = await m(); await pg.waitForTimeout(5000); const b = await m();
    const fps = await pg.evaluate(() => { const f = document.querySelector('#ovBox iframe'); const d = f ? f.contentDocument : document; return d.querySelector('#fpsV') && d.querySelector('#fpsV').textContent; });
    console.log(`${name}: busy ${((b.TaskDuration - a.TaskDuration) * 1000 / 5).toFixed(0)} ms/s, script ${((b.ScriptDuration - a.ScriptDuration) * 1000 / 5).toFixed(0)} ms/s, ${fps}`);
    await ctx.close();
  }
  await br.close();
})();
