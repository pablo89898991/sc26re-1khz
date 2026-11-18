/* WebHID stand-in: a virtual Steam Controller served by sim_server.py */
(()=>{
  const SRV = "http://127.0.0.1:8765";
  const ls = {};
  let reply = null;
  const dev = {
    vendorId: 0x28de, productId: 0x1302, productName: "Virtual Steam Controller", opened: false,
    collections: [{ usagePage: 0xff00, featureReports: [{ reportId: 1 }], inputReports: [{ reportId: 0x42 }] }],
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    addEventListener(t, f) { (ls[t] = ls[t] || []).push(f); },
    removeEventListener(t, f) { ls[t] = (ls[t] || []).filter(x => x !== f); },
    async sendFeatureReport(id, buf) {
      const b = new Uint8Array(buf.buffer || buf, buf.byteOffset || 0, buf.byteLength);
      const hex = Array.from(b).map(x => x.toString(16).padStart(2, "0")).join("");
      const r = await (await fetch(SRV + "/feature", { method: "POST", body: hex })).json();
      if (b[0] === 0xED) { const h = r.rsp; reply = new Uint8Array(h.match(/../g).map(x => parseInt(x, 16))); }
      else if (r.r < 0 && r.r !== -2) throw new DOMException("failed " + r.r);
    },
    async receiveFeatureReport(id) {
      if (!reply) throw new DOMException("no data");
      const r = reply; return new DataView(r.buffer);
    },
  };
  window.__vdev = dev;
  const hid = {
    async requestDevice() { return [dev]; },
    async getDevices() { return dev.opened ? [dev] : []; },
    addEventListener() {}, removeEventListener() {},
  };
  Object.defineProperty(navigator, "hid", { value: hid, configurable: true });
  setInterval(async () => {
    if (!dev.opened) return;
    try {
      const h = await (await fetch(SRV + "/report")).text();
      const b = new Uint8Array(1 + h.length / 2); b.set(h.match(/../g).map(x => parseInt(x, 16)), 1);
      const ev = { reportId: 0x42, device: dev, data: new DataView(b.buffer) };
      (ls.inputreport || []).forEach(f => f(ev));
    } catch (e) {}
  }, 15);
})();
