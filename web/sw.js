/* Offline: every page and file of the tools is kept by the browser after the first visit.
 * Online the newest version is fetched first (so an update shows at once) and stored; offline the
 * stored copy is used. Nothing here talks to anything but this site. */
const CACHE = "sc26re-tools-v1";
const CORE = ["./", "index.html", "remap.html", "meter.html", "overlay.html", "patcher.html", "fwtest.html", "hwtest.html",
  "actnames.js", "overlay_settings.js", "vdf.js", "stickcal.js", "manifest.webmanifest", "icon-192.png", "icon-512.png",
  "vendor/three/build/three.module.min.js", "vendor/three/examples/jsm/loaders/GLTFLoader.js",
  "vendor/three/examples/jsm/libs/meshopt_decoder.module.js", "vendor/three/examples/jsm/utils/BufferGeometryUtils.js",
  "models/steam-controller-valve.glb", "models/steam-controller.glb", "hwtests.json"];
self.addEventListener("install", e => { e.waitUntil(caches.open(CACHE).then(c => Promise.all(CORE.map(u => c.add(u).catch(() => {})))).then(() => self.skipWaiting())); });
self.addEventListener("activate", e => { e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim())); });
self.addEventListener("fetch", e => {
  const req = e.request, url = new URL(req.url);
  if (req.method !== "GET" || url.origin !== location.origin) return;
  e.respondWith((async () => {
    const c = await caches.open(CACHE);
    try {
      const r = await fetch(req, { cache: "no-cache" });
      if (r.ok && r.type === "basic") c.put(new Request(url.origin + url.pathname), r.clone()).catch(() => {});
      return r;
    } catch (err) {
      /* offline: the stored copy, whatever the query (?v=…, cache busters) */
      return (await c.match(req, { ignoreSearch: true })) || (req.mode === "navigate" && await c.match(new URL("index.html", location.href).href)) || Response.error();
    }
  })());
});
