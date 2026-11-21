// 3D overlay (web/overlay.html) without a controller: Valve's D-pad tilts in place, pop-out of
// triggers / bumpers / back buttons, grip sense drawn over the body, gyro speed, heading pull,
// frame rate limit. Needs the web folder served on 8766. V=1 prints every check.
const { chromium } = require('/opt/npm-tools/node_modules/playwright');
let bad = 0, total = 0;
const check = (name, ok, info = '') => { total++; if (!ok) bad++; if (!ok || process.env.V) console.log((ok ? 'PASS ' : 'FAIL ') + name + (info ? ' — ' + info : '')); };
(async () => {
  const b = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const pg = await b.newPage({ viewport: { width: 1000, height: 700 } });
  const errs = []; pg.on('pageerror', e => errs.push(e.message));
  await pg.goto('http://127.0.0.1:8766/overlay.html#debug');
  await pg.evaluate(() => localStorage.clear()); await pg.reload();
  await pg.waitForFunction(() => window.__ov && __ov.ready, null, { timeout: 60000 });
  const ev = (f, a) => pg.evaluate(f, a);
  const DP = { up: 13, down: 10, left: 12, right: 11 };
  for (const [arm, bit] of Object.entries(DP)) {
    const r = await ev(([arm, bit]) => new Promise(res => {
      const P = n => { const m = __ov.parts.get(n).pivot.children[0], g = m.geometry; g.computeBoundingBox(); const v = g.boundingBox.min.clone().add(g.boundingBox.max).multiplyScalar(0.5); __ov.model.updateMatrixWorld(true); return m.localToWorld(v); };
      const names = ['up', 'down', 'left', 'right'].map(a => 'dpad_' + a), before = names.map(P);
      __ov.st.btn = 1 << bit;
      setTimeout(() => { const after = names.map(P); __ov.st.btn = 0; res(Math.max(...after.map((v, i) => v.distanceTo(before[i])))); }, 200);
    }), [arm, bit]);
    check(`Valve D-pad ${arm}: tilts in place`, r < 0.35, `an arm moved ${r.toFixed(3)} cm`);
  }
  const pop = await ev(() => { const P = n => { const v = __ov.model.position.clone(); __ov.parts.get(n).pivot.getWorldPosition(v); return v; };
    __ov.model.updateMatrixWorld(true);
    const box = new (__ov.model.position.constructor)(); return { r4: P('back_r4').x, l4: P('back_l4').x, trig: P('trigger_l').y, bump: P('bumper_l').y, body: (() => { let mx = 0; __ov.model.traverse(o => { if (o.isMesh && /shell/.test(o.name)) { o.geometry.computeBoundingBox(); mx = Math.max(mx, o.geometry.boundingBox.max.x * o.getWorldScale(box).x); } }); return mx; })() }; });
  check('back buttons popped out beside the body', pop.r4 > pop.body * 0.95 && pop.l4 < -pop.body * 0.95, JSON.stringify(pop));
  check('triggers above the bumpers', pop.trig > pop.bump, JSON.stringify(pop));
  for (const mdl of ['valve', 'detail']) {
    const r = await ev(mdl => new Promise(res => { const sel = document.querySelector('#mdl'); if (sel.value !== mdl) { sel.value = mdl; sel.dispatchEvent(new Event('change')); }
      const go = () => { if (!__ov.ready) return setTimeout(go, 100); setTimeout(() => { __ov.model.updateMatrixWorld(true);
        const B = o => new (__ov.model.position.constructor === undefined ? Object : Object)() && (() => { const b = { min: [1e9, 1e9], max: [-1e9, -1e9] }; o.traverse(m => { if (!m.isMesh) return; const g = m.geometry; g.computeBoundingBox();
          for (const x of [g.boundingBox.min.x, g.boundingBox.max.x]) for (const y of [g.boundingBox.min.y, g.boundingBox.max.y]) for (const z of [g.boundingBox.min.z, g.boundingBox.max.z]) { const v = m.localToWorld(__ov.model.position.clone().set(x, y, z)); b.min[0] = Math.min(b.min[0], v.x); b.max[0] = Math.max(b.max[0], v.x); b.min[1] = Math.min(b.min[1], v.y); b.max[1] = Math.max(b.max[1], v.y); } }); return b; })();
        let body = null; __ov.model.traverse(o => { if (o.isMesh && (/shell|^body$/.test(o.name))) { const b = B(o); body = body ? { min: [Math.min(body.min[0], b.min[0]), Math.min(body.min[1], b.min[1])], max: [Math.max(body.max[0], b.max[0]), Math.max(body.max[1], b.max[1])] } : b; } });
        const bl = B(__ov.parts.get('bumper_l').pivot), br = B(__ov.parts.get('bumper_r').pivot), tl = B(__ov.parts.get('trigger_l').pivot), tr = B(__ov.parts.get('trigger_r').pivot);
        res({ body, bl, br, tl, tr, rz: __ov.parts.get('back_r4').pivot.rotation.z }); }, 300); }; go(); }), mdl);
    const cx = b => (b.min[0] + b.max[0]) / 2, cy = b => (b.min[1] + b.max[1]) / 2;
    const under = (b, t) => Math.abs(cx(b) - cx(t)) < 1.0 && cy(b) < cy(t) && b.max[1] > r.body.max[1] - 0.5;
    check(`${mdl}: popped bumpers right under their triggers, above the body`, under(r.bl, r.tl) && under(r.br, r.tr), JSON.stringify(r));
    check(`${mdl}: both bumpers at one height`, Math.abs(cy(r.bl) - cy(r.br)) < 0.01 && Math.abs(cy(r.tl) - cy(r.tr)) < 0.01);
    check(`${mdl}: back buttons turned their marked face to the camera`, Math.abs(r.rz) > 3, String(r.rz));
  }
  await ev(() => { const sel = document.querySelector('#mdl'); sel.value = 'valve'; sel.dispatchEvent(new Event('change')); });
  await pg.waitForFunction(() => __ov.ready, null, { timeout: 60000 });
  const grip = await ev(() => new Promise(res => { __ov.st.btn = 1 << 29; setTimeout(() => { const p = __ov.parts.get('grip_l'); const r = [p.material.opacity, p.material.depthTest]; __ov.st.btn = 0; setTimeout(() => res([...r, __ov.parts.get('grip_l').material.opacity]), 100); }, 100); }));
  check('grip sense held: drawn over the body', grip[0] === 0.85 && grip[1] === false && grip[2] === 0.12, JSON.stringify(grip));
  const ang = q => 2 * Math.acos(Math.min(1, Math.abs(q[0]))) * 180 / Math.PI;
  for (const s of [50, 100, 200]) {
    const a = await ev(s => { const e = document.querySelector('#sens'); e.value = s; e.dispatchEvent(new Event('input')); __ov.q = [1, 0, 0, 0]; __ov.gyroStep(40, 0, 0, 1); return __ov.q; }, s);
    check(`gyro speed ${s / 100}x`, Math.abs(ang(a) - 40 * s / 100) < 0.01, ang(a).toFixed(3));
  }
  await ev(() => { const e = document.querySelector('#sens'); e.value = 100; e.dispatchEvent(new Event('input')); });
  const yaw = await ev(() => { document.querySelector('#yawFix').checked = true; const k = document.querySelector('#yawK'); k.value = 100; k.dispatchEvent(new Event('input'));
    __ov.q = [1, 0, 0, 0]; __ov.recenter(); __ov.q = [Math.cos(0.3), 0, 0, Math.sin(0.3)]; const a0 = __ov.q.slice(); __ov.yawStep(0.5); const a1 = __ov.q.slice(); __ov.yawStep(5); const a2 = __ov.q.slice();
    document.querySelector('#yawFix').checked = false; __ov.q = a0.slice(); __ov.yawStep(1); return [a0, a1, a2, __ov.q]; });
  check('heading pull: part way after half its time', ang(yaw[1]) < ang(yaw[0]) - 10 && ang(yaw[1]) > 5, yaw.map(ang).map(x => x.toFixed(2)).join(' '));
  check('heading pull: back to centre', ang(yaw[2]) < 0.01, ang(yaw[2]).toFixed(3));
  check('heading pull off: nothing moves', Math.abs(ang(yaw[3]) - ang(yaw[0])) < 1e-9);
  const tilted = await ev(() => { document.querySelector('#yawFix').checked = true; __ov.q = [1, 0, 0, 0]; __ov.recenter();
    const s = Math.sin(0.25), c = Math.cos(0.25); __ov.q = [c, s, 0, 0]; const a = __ov.q.slice(); __ov.yawStep(5); document.querySelector('#yawFix').checked = false; return [a, __ov.q]; });
  check('heading pull leaves a pitch alone', Math.abs(ang(tilted[1]) - ang(tilted[0])) < 1e-6, tilted.map(ang).join(' '));
  for (const f of [30, 60]) {
    await ev(f => { const e = document.querySelector('#fps'); e.value = f; e.dispatchEvent(new Event('input')); }, f);
    await pg.waitForTimeout(2300);
    const t = await ev(() => document.querySelector('#fpsV').textContent);
    /* headless drawing is slow: 60 is a ceiling here */
    check(`frame rate limit ${f}`, f === 30 ? Math.abs(parseInt(t) - 30) <= 3 : parseInt(t) <= 62 && parseInt(t) >= 20, t);
  }
  const saved = await ev(() => { const o = JSON.parse(localStorage.getItem('sc26re_overlay2') || '{}'); return [o.fps, o.sens, o.yawFix, o.popBack]; });
  check('settings are kept', saved[0] === '60' && saved[1] === '100' && saved[3] === true, JSON.stringify(saved));
  check('no page errors', errs.length === 0, errs.join(' | '));
  console.log(`${total} checks, ${bad} failed`);
  await b.close(); process.exit(bad ? 1 : 0);
})();
