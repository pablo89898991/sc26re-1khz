/* Stick calibration (sc26re v51+): shared by the remapper and the report meter.
 * Firmware: "sc26re/stick_cal/start|finish|cancel" (command), "sc26re/stick_cal" (32-byte state:
 * stage, result, samples, then per axis LX LY RX RY: now, low, high in mV).
 * stickCalMount(el, { read(path) -> Uint8Array, cmd(path) -> Promise }) builds the card body. */
function stickCalMount(el, io) {
  el.innerHTML = `<p class="note cardnote">Full range: <b>Start</b> → hands off both sticks for a second (the centre is measured) → turn <b>both sticks</b> slowly round their edge 2–3 times, pushing all the way → <b>Finish &amp; save</b>. The circles fill in as you go. Saved in the controller; Steam uses it too.</p>
  <canvas width="300" height="140" style="width:300px;height:140px;background:#0b1220;border-radius:8px;display:block;margin:4px 0"></canvas>
  <div class="tools"><button class="primary" data-a="start">Start</button><button data-a="finish" disabled>Finish &amp; save</button><button data-a="cancel" class="ghost" disabled>Cancel</button></div>
  <p class="note cardnote" data-s></p>`;
  const cv = el.querySelector("canvas"), g = cv.getContext("2d"), st = el.querySelector("[data-s]");
  const B = a => el.querySelector(`[data-a=${a}]`);
  let run = false, trail = [[], []];
  const u16 = (b, i) => b[i] | b[i + 1] << 8;
  function draw(s) {
    g.clearRect(0, 0, 300, 140);
    for (let k = 0; k < 2; k++) {
      const cx = 75 + k * 150, cy = 70, r = 56;
      g.strokeStyle = "#334155"; g.lineWidth = 1; g.beginPath(); g.arc(cx, cy, r, 0, 7); g.stroke();
      g.fillStyle = "#94a3b8"; g.font = "11px system-ui"; g.fillText(k ? "right" : "left", cx - 12, 136);
      if (!s) continue;
      const ax = [k * 2, k * 2 + 1], lo = ax.map(i => s.lo[i]), hi = ax.map(i => s.hi[i]);
      const n = (v, i) => hi[i] > lo[i] ? ((v - lo[i]) / (hi[i] - lo[i])) * 2 - 1 : 0;
      g.strokeStyle = "#38bdf8"; g.lineWidth = 2; g.beginPath();
      trail[k].forEach(([x, y], j) => { const px = cx + n(x, 0) * r, py = cy - n(y, 1) * r; j ? g.lineTo(px, py) : g.moveTo(px, py); });
      g.stroke();
      const now = [s.now[ax[0]], s.now[ax[1]]];
      g.fillStyle = "#f8fafc"; g.beginPath(); g.arc(cx + n(now[0], 0) * r, cy - n(now[1], 1) * r, 4, 0, 7); g.fill();
    }
  }
  async function state() {
    const b = await io.read("sc26re/stick_cal");
    const s = { stage: b[0], result: b[1] > 127 ? b[1] - 256 : b[1], n: u16(b, 2), now: [], lo: [], hi: [] };
    for (let i = 0; i < 4; i++) { s.now.push(u16(b, 4 + i * 2)); s.lo.push(u16(b, 12 + i * 2)); s.hi.push(u16(b, 20 + i * 2)); }
    return s;
  }
  function buttons(stage) { B("start").disabled = stage > 0; B("finish").disabled = stage !== 2; B("cancel").disabled = stage === 0; }
  async function loop() {
    while (run) {
      let s; try { s = await state(); } catch (e) { st.innerHTML = `<span class="err">${e.message}</span>`; run = false; break; }
      if (s.stage === 2) for (let k = 0; k < 2; k++) { trail[k].push([s.now[k * 2], s.now[k * 2 + 1]]); if (trail[k].length > 3000) trail[k].shift(); }
      draw(s); buttons(s.stage);
      if (s.stage === 1) st.textContent = "hands off the sticks — measuring the centre…";
      else if (s.stage === 2) {
        const span = i => s.hi[i] - s.lo[i], ok = [0, 1, 2, 3].every(i => span(i) > 900);
        st.innerHTML = "turn both sticks round their edge, all the way out" + (ok ? ' — <span class="ok">looks complete: Finish &amp; save</span>' : "");
      } else { run = false; }
      await new Promise(r => setTimeout(r, 40));
    }
  }
  B("start").onclick = async () => {
    try { await io.cmd("sc26re/stick_cal/start"); trail = [[], []]; run = true; buttons(1); loop(); }
    catch (e) { st.innerHTML = `<span class="err">${e.message} (needs sc26re v51 or newer)</span>`; }
  };
  B("cancel").onclick = async () => { run = false; try { await io.cmd("sc26re/stick_cal/cancel"); } catch (e) {} buttons(0); st.textContent = "cancelled, nothing changed"; };
  B("finish").onclick = async () => {
    run = false;
    let err = null; try { await io.cmd("sc26re/stick_cal/finish"); } catch (e) { err = e; }
    let s = null; try { s = await state(); } catch (e) {}
    buttons(0);
    const r = s ? s.result : -1, side = ["", "left", "right", "both sticks"];
    if (r > 0 && r < 4) st.innerHTML = `<span class="ok">saved: ${side[r]}</span>` + (r !== 3 ? ' — the other stick did not move far enough, it keeps its old calibration' : "");
    else st.innerHTML = `<span class="err">not saved: the sticks did not reach far enough from the centre${err ? " (" + err.message + ")" : ""}. Start again and push all the way round.</span>`;
  };
  draw(null);
}
