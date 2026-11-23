/* Controller battery (firmware v51.4+: feature path sc26re/battery), shared by the report meter,
 * the remapper and the overlay (which hands it to the SC2 Overlay app's tray tooltip).
 * Payload: [0] 1 fresh / 2 stale / 0 none, [1] charge state, [2] level %, [3..4] mV,
 * [5..6] charge current mA, [7..8] temperature 0.1 C (int16), [9] charger type,
 * [10] charge done, [11] fuel gauge state. */
(function () {
  const STATE = { 0: "starting", 1: "on battery", 2: "charging", 3: "checking the charger", 4: "full" };
  function decode(b) {
    if (!b || b.length < 12 || !b[0]) return null;
    const u16 = o => b[o] | (b[o + 1] << 8), s16 = o => (u16(o) << 16) >> 16;
    const st = b[1];
    return {
      fresh: b[0] === 1, state: st, stateName: STATE[st] || "unknown",
      charging: st === 2 || st === 3, full: st === 4 || b[10] === 1,
      pct: Math.min(100, b[2]), mv: u16(3), ma: u16(5), tempC: s16(7) / 10, charger: b[9], done: !!b[10],
    };
  }
  /* short text for a badge, long one for a tooltip */
  function short(x) {
    if (!x) return "";
    return (x.charging ? "⚡" : "🔋") + " " + x.pct + "%" + (x.full && x.charging === false && x.state === 4 ? " full" : "");
  }
  function long(x) {
    if (!x) return "Battery: not reported (firmware v51.4 or newer shows it)";
    const parts = [`Battery ${x.pct}%`, x.stateName];
    if (x.mv) parts.push((x.mv / 1000).toFixed(2) + " V");
    if (x.charging && x.ma) parts.push(x.ma + " mA");
    if (x.tempC) parts.push(x.tempC.toFixed(1) + " °C");
    if (!x.fresh) parts.push("(old reading)");
    return parts.join(" · ");
  }
  function color(x) { return !x ? "" : x.charging || x.full ? "#5fd38d" : x.pct <= 15 ? "#ff6b6b" : x.pct <= 30 ? "#ffcc66" : ""; }
  /* read(path) -> Promise<Uint8Array>; calls show(info|null) now and every ms (default 30 s) */
  function poll(read, show, ms) {
    let busy = false;
    const tick = async () => {
      if (busy) return; busy = true;
      try { show(decode(await read("sc26re/battery"))); } catch (e) { /* not connected / busy */ }
      busy = false;
    };
    tick(); return setInterval(tick, ms || 30000);
  }
  /* fills a <span>: text, tooltip, colour */
  function paint(el, x) {
    if (!el) return;
    el.hidden = !x; el.textContent = short(x); el.title = long(x);
    el.style.color = color(x);
  }
  window.SCB = { decode, short, long, color, poll, paint };
})();
