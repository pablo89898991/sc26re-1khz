/* WebHID stand-in: a virtual Steam Controller served by sim_server.py */
(()=>{
  const SRV = "http://127.0.0.1:8765";
  /* a slow, lossy link like the Puck: delays, refused sends, lost replies, and sends
   * that reach the controller although the page gets an error */
  const rnd = () => Math.random();
  const delay = () => new Promise(r => setTimeout(r, 5 + rnd() * 35));
  window.__linkStats = { refused: 0, ghost: 0, lostReply: 0, stale: 0 };
  const ls = {};
  let reply = null;
  const dev = {
    vendorId: 0x28de, productId: 0x1302, productName: "Virtual Steam Controller", opened: false,
    collections: [{ usagePage: 0xff00, featureReports: [{ reportId: 1 }], inputReports: [{ reportId: 0x42 }] }],
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    addEventListener(t, f) { const a = (ls[t] = ls[t] || []); if (!a.includes(f)) a.push(f); },
    removeEventListener(t, f) { ls[t] = (ls[t] || []).filter(x => x !== f); },
    async sendFeatureReport(id, buf) {
      await delay();
      if (rnd() < .06) { __linkStats.refused++; throw new DOMException("Failed to write the feature report."); }
      const b = new Uint8Array(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength);
      const hex = Array.from(b).map(x => x.toString(16).padStart(2, "0")).join("");
      const r = await (await fetch(SRV + "/feature", { method: "POST", body: hex })).json();
      if (b[0] === 0xED) { const h = r.rsp; reply = new Uint8Array(h.match(/../g).map(x => parseInt(x, 16))); if (rnd() < .08) { __linkStats.lostReply++; reply = null; } }
      if (b[0] !== 0xED && rnd() < .04) { __linkStats.ghost++; throw new DOMException("Failed to write the feature report."); } // it did arrive
      else if (r.r < 0 && r.r !== -2) throw new DOMException("failed " + r.r);
    },
    async receiveFeatureReport(id) {
      await delay();
      if (!reply) throw new DOMException("no data");
      /* the controller has one reply buffer: another page's request may have replaced it.
       * Asked too early over the Puck or Bluetooth, the previous reply comes back. */
      const stale = rnd() < .1; if (stale) __linkStats.stale++;
      const h = await (await fetch(SRV + (stale ? "/reply_prev" : "/reply"))).text();
      const r = h ? new Uint8Array(h.match(/../g).map(x => parseInt(x, 16))) : reply; return new DataView(r.buffer);
    },
  };
  window.__vdev = dev;
  const hidLs = {};
  const hid = {
    async requestDevice() { return [dev]; },
    async getDevices() { return dev.opened ? [dev] : []; },
    addEventListener(t, f) { const a = (hidLs[t] = hidLs[t] || []); if (!a.includes(f)) a.push(f); },
    removeEventListener(t, f) { hidLs[t] = (hidLs[t] || []).filter(x => x !== f); },
  };
  /* a test unplugs the controller and plugs it in again (the same device the page may use) */
  window.__unplug = () => { dev.opened = false; (hidLs.disconnect || []).forEach(f => f({ device: dev })); };
  window.__plug = () => { (hidLs.connect || []).forEach(f => f({ device: dev })); };
  Object.defineProperty(navigator, "hid", { value: hid, configurable: true });
  /* every pad report, in order, like WebHID (the server runs the controller in 5 ms steps);
   * now and then one is lost on the way */
  let seen = -1;
  (async function poll() {
    for (;;) {
      await new Promise(r => setTimeout(r, 15));
      if (!dev.opened) continue;
      try {
        const j = await (await fetch(SRV + "/reports42?after=" + seen)).json(); seen = j.n;
        for (const h of j.r) {
          if (rnd() < .02) continue;
          const b = new Uint8Array(h.match(/../g).map(x => parseInt(x, 16)));
          const ev = { reportId: 0x42, device: dev, data: new DataView(b.buffer) };
          (ls.inputreport || []).forEach(f => f(ev));
        }
      } catch (e) {}
    }
  })();
})();
