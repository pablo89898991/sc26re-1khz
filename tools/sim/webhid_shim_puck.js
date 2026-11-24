/* The Puck as Chrome shows it: several HID interfaces, only one takes the controller's requests.
 * Loads after webhid_shim.js: puts interfaces that refuse every write in front of the real one. */
(()=>{
  const real = window.__vdev, hid = navigator.hid;
  const fake = n => ({ vendorId: 0x28de, productId: 0x1304, productName: "Steam Controller Puck", opened: false, __fake: n,
    collections: [{ usagePage: 0xff00, featureReports: [{ reportId: 1 }, { reportId: 2 }], inputReports: [{ reportId: 0x42 }] }],
    async open() { this.opened = true; }, async close() { this.opened = false; },
    addEventListener() {}, removeEventListener() {},
    async sendFeatureReport() { await new Promise(r => setTimeout(r, 5)); throw new DOMException("Failed to write the feature report."); },
    async receiveFeatureReport() { throw new DOMException("Failed to receive the feature report."); } });
  const list = [fake(0), fake(1), real, fake(3), fake(4)];
  const req = hid.requestDevice.bind(hid), get = hid.getDevices.bind(hid);
  hid.requestDevice = async o => { await req(o); return list; };
  hid.getDevices = async () => { const g = await get(); return g.length ? list.filter(d => d !== real || g.includes(real)) : []; };
})();
