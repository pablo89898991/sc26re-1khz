// SC2 overlay for Windows and Linux: the overlay page (web/overlay.html) in a see-through window over the
// whole screen, always on top and letting every click through to the game. The tray icon
// arranges it, opens the remapper (its Overlay tab sets the overlay up live), picks the screen.
const { app, BrowserWindow, Tray, Menu, screen, session, protocol, net, globalShortcut, ipcMain, nativeImage, shell, Notification } = require("electron");
const path = require("path");
const fs = require("fs");
const { pathToFileURL } = require("url");

const WEB = app.isPackaged ? path.join(process.resourcesPath, "web") : path.join(__dirname, "..", "web");
const ORIGIN = "app://local";
const ICON = path.join(__dirname, "icon.png");
const CONF = path.join(app.getPath("userData"), "app.json");
const VALVE = 0x28de;
const KEY_ARRANGE = "CommandOrControl+Shift+F10";
const KEY_SHOW = "CommandOrControl+Shift+F9";
const LINUX = process.platform === "linux";

let conf = { display: null, shown: true, updates: true, told: {}, fastGpu: true };
try { conf = Object.assign(conf, JSON.parse(fs.readFileSync(CONF, "utf8"))); } catch (e) {}
const saveConf = () => { try { fs.writeFileSync(CONF, JSON.stringify(conf)); } catch (e) {} };

/* the pages come from a secure origin of their own (WebHID needs one; localStorage is shared, so the
 * remapper window's Overlay tab and the overlay window see each other's settings) */
protocol.registerSchemesAsPrivileged([{ scheme: "app", privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true } }]);

if (!app.requestSingleInstanceLock()) { app.quit(); }
if (process.platform === "win32") app.setAppUserModelId("io.github.pablo89898991.sc26re-overlay");
app.commandLine.appendSwitch("disable-renderer-backgrounding");
/* 1.4: draw on the fast graphics card. On a laptop with two (an integrated one and NVIDIA / AMD) the
 * app ran on the integrated one while the game ran on the fast one, and every overlay frame had to be
 * copied from one card to the other for Windows to blend it over the game: that made the overlay and
 * the game stutter. 3D Controller Overlay ran on the fast card. Tray: "Use the fast graphics card". */
if (conf.fastGpu) app.commandLine.appendSwitch("force_high_performance_gpu");
/* Linux: run through X11 (XWayland on a Wayland session). A native Wayland window cannot be always on top,
 * placed at a fixed position, or click-through the same way. SC2_OVERLAY_WAYLAND=1 opts out. */
if (LINUX && !process.env.SC2_OVERLAY_WAYLAND) app.commandLine.appendSwitch("ozone-platform", "x11");

/* "Start at login": Electron's login item only covers Windows and macOS, so on Linux it is an XDG autostart
 * entry (~/.config/autostart). An AppImage starts through its own path, else the installed binary. */
const AUTOSTART = path.join(process.env.XDG_CONFIG_HOME || path.join(require("os").homedir(), ".config"), "autostart", "sc2-overlay.desktop");
function getAutostart() {
  if (!LINUX) return app.getLoginItemSettings().openAtLogin;
  return fs.existsSync(AUTOSTART);
}
function setAutostart(on) {
  if (!LINUX) return app.setLoginItemSettings({ openAtLogin: on });
  try {
    if (!on) { fs.rmSync(AUTOSTART, { force: true }); return; }
    fs.mkdirSync(path.dirname(AUTOSTART), { recursive: true });
    const exe = (process.env.APPIMAGE || process.execPath).replace(/"/g, '\\"');
    fs.writeFileSync(AUTOSTART, ["[Desktop Entry]", "Type=Application", "Name=SC2 Overlay", "Comment=Steam Controller 2026 overlay",
      `Exec="${exe}"`, "X-GNOME-Autostart-enabled=true", ""].join("\n"));
  } catch (e) { console.warn("autostart:", e.message); }
}

/* 1.4: two windows. overlay = the menus, over the whole screen, shown only while a menu, the profile
 * name or the arrange mode is on it; modelWin = a small window only around the 3D controller.
 * A see-through window over the whole screen made the game's frames slower (Windows blends all of
 * it every frame); a small one costs about what 3D Controller Overlay's window does. */
let overlay = null, modelWin = null, tray = null, editing = false, menuWanted = false;
const pages = {}; /* page file -> its window (remapper, report meter, ...) */

function pickDisplay() {
  const all = screen.getAllDisplays();
  return all.find(d => d.id === conf.display) || screen.getPrimaryDisplay();
}

function seeThrough(bounds, part) {
  const w = new BrowserWindow({
    x: bounds.x, y: bounds.y, width: bounds.width, height: bounds.height,
    transparent: true, backgroundColor: "#00000000", frame: false, hasShadow: false, resizable: false, movable: false,
    skipTaskbar: true, focusable: false, alwaysOnTop: true, fullscreenable: false, show: false, icon: ICON,
    webPreferences: { preload: path.join(__dirname, "preload.js"), backgroundThrottling: false },
  });
  w.setAlwaysOnTop(true, "screen-saver");
  w.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
  w.setIgnoreMouseEvents(true);
  w.loadURL(ORIGIN + "/overlay.html?pip=1&app=1&part=" + part);
  return w;
}
/* the 3D controller's area: size % of the screen around (x %, y %), like the page places it */
function modelBounds() {
  const d = pickDisplay().bounds, r = conf.model || { x: 85, y: 78, size: 30, on: true }, k = Math.max(0.05, Math.min(1, r.size / 100));
  const w = Math.max(40, Math.round(d.width * k)), h = Math.max(40, Math.round(d.height * k));
  return { x: Math.round(d.x + d.width * r.x / 100 - w / 2), y: Math.round(d.y + d.height * r.y / 100 - h / 2), width: w, height: h };
}
function showModel() {
  if (!modelWin) return;
  const on = conf.shown && (!conf.model || conf.model.on !== false);
  if (on && !modelWin.isVisible()) modelWin.showInactive(); else if (!on && modelWin.isVisible()) modelWin.hide();
}
function showMenu() {
  if (!overlay) return;
  const on = editing || (conf.shown && menuWanted);
  if (on && !overlay.isVisible()) { overlay.showInactive(); overlay.moveTop(); } else if (!on && overlay.isVisible()) overlay.hide();
}
function makeOverlay() {
  overlay = seeThrough(pickDisplay().bounds, "menu");
  overlay.on("closed", () => { overlay = null; });
  modelWin = seeThrough(modelBounds(), "model");
  modelWin.once("ready-to-show", showModel);
  modelWin.on("closed", () => { modelWin = null; });
}

function placeOverlay() {
  if (overlay) overlay.setBounds(pickDisplay().bounds);
  if (modelWin) modelWin.setBounds(modelBounds());
}

function setEditing(on) {
  editing = on;
  /* Linux: the overlay window gets no keyboard focus, so Esc is a global key while arranging (X11 only) */
  if (LINUX) { if (on) globalShortcut.register("Escape", toggleArrange); else globalShortcut.unregister("Escape"); }
  if (!overlay) return;
  overlay.setIgnoreMouseEvents(!on);
  overlay.setFocusable(on);
  showMenu();
  if (on) overlay.focus();
  overlay.setAlwaysOnTop(true, "screen-saver");
  buildTray();
}

function toggleArrange() {
  if (!overlay) return;
  overlay.webContents.executeJavaScript(`window.__ovEdit && window.__ovEdit(${!editing})`).catch(() => {});
}

function toggleShown() {
  conf.shown = !conf.shown; saveConf();
  if (!conf.shown && editing) toggleArrange();
  showMenu(); showModel();
  buildTray();
}

/* the tools pages, all inside the app (they work without the internet) */
const PAGES = [["remap.html", "Remapper and overlay settings…", 1320, 940], ["meter.html", "Report meter and flasher…", 1320, 940],
  ["overlay.html", "3D overlay page (OBS)…", 1100, 760], ["patcher.html", "Puck patcher…", 1100, 860], ["index.html", "All tools…", 1000, 760]];
function openPage(file) {
  const w0 = pages[file];
  if (w0) { w0.show(); w0.focus(); return; }
  const [, title, width, height] = PAGES.find(p => p[0] === file) || [file, file, 1200, 900];
  const w = new BrowserWindow({ width, height, icon: ICON, title: title.replace("…", ""), autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, "preload.js") } });
  pages[file] = w;
  w.loadURL(ORIGIN + "/" + file);
  /* links between the tools stay in the app; others (help, licences) open in the normal browser */
  w.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith(ORIGIN)) return { action: "allow", overrideBrowserWindowOptions: { autoHideMenuBar: true, icon: ICON, webPreferences: { preload: path.join(__dirname, "preload.js") } } };
    shell.openExternal(url); return { action: "deny" };
  });
  w.webContents.on("will-navigate", (e, url) => { if (!url.startsWith(ORIGIN)) { e.preventDefault(); shell.openExternal(url); } });
  w.on("closed", () => { delete pages[file]; });
}
const openRemap = () => openPage("remap.html");


/* ---------- update check: GitHub releases of the project, once at start and every 6 hours ----------
 * Only reads the public release list (no download, nothing installed): a notification and a tray
 * entry that open the release page. Each new version is announced once. */
const REPO = "pablo89898991/sc26re-1khz";
/* controller battery from the overlay page (tray tooltip + menu line); null: none reported */
let batt = null, lowTold = false;
const RELEASES = `https://github.com/${REPO}/releases`;
const upd = { app: null, fw: null, fwHave: undefined, checked: 0, error: "" };
/* --updates-test: act as app 1.0.0 with a controller on v50, so both notifications show (testing) */
const UPD_TEST = process.argv.includes("--updates-test");
const appVer = () => UPD_TEST ? "1.0.0" : app.getVersion();
/* "v51.2" / "overlay-v1.10.0" -> [51, 2] / [1, 10, 0]; null if it is not a version */
const verOf = t => { const m = /^(?:overlay-)?v(\d+(?:\.\d+)*)$/.exec(t || ""); return m ? m[1].split(".").map(Number) : null; };
const verCmp = (a, b) => { for (let i = 0; i < Math.max(a.length, b.length); i++) { const d = (a[i] || 0) - (b[i] || 0); if (d) return d; } return 0; };
function tell(key, title, body, url) {
  /* test mode: once per run, not remembered */
  if (UPD_TEST) { if (tell.run[key]) return; tell.run[key] = 1; }
  else { if (conf.told[key]) return; conf.told[key] = 1; saveConf(); }
  notify(title, body, url);
}
/* Windows: a toast from a portable .exe without a Start menu shortcut is dropped silently, so the
 * tray icon's own balloon is used there (Windows 10/11 show it as a normal notification). One at a
 * time: a second balloon would replace the first, so they wait in a queue. */
const balloons = []; let balloonUrl = null, balloonT = null;
function nextBalloon() {
  clearTimeout(balloonT); balloonT = null;
  const b = balloons.shift(); if (!b || !tray) return; /* the last one stays clickable in the notification centre */
  balloonUrl = b.url;
  tray.displayBalloon({ title: b.title, content: b.body, icon: nativeImage.createFromPath(ICON), respectQuietTime: false });
  balloonT = setTimeout(nextBalloon, 8000);
}
function notify(title, body, url) {
  if (process.platform === "win32") {
    balloons.push({ title, body, url });
    if (!balloonT) nextBalloon();
    return;
  }
  if (!Notification.isSupported()) return;
  const n = new Notification({ title, body, icon: ICON });
  if (url) n.on("click", () => shell.openExternal(url));
  n.show();
}
tell.run = {};
function announce() {
  if (!conf.updates) return;
  const mine = verOf("v" + appVer());
  if (upd.app && verCmp(verOf(upd.app.tag), mine) > 0)
    tell("app " + upd.app.tag, "SC2 Overlay update", `Version ${upd.app.tag.replace("overlay-v", "")} is out (you have ${appVer()}). Click to open the download page.`, upd.app.url);
  /* the firmware: only once the overlay has read the controller's version (undefined = no controller yet) */
  if (upd.fw && upd.fwHave !== undefined) {
    const have = verOf(upd.fwHave);
    if (!have || verCmp(verOf(upd.fw.tag), have) > 0)
      tell("fw " + upd.fw.tag, "sc26re firmware update", `Firmware ${upd.fw.tag} is out (your controller has ${upd.fwHave || "an older version"}). Click to open the release page.`, upd.fw.url);
  }
}
async function checkUpdates(manual) {
  try {
    const r = await net.fetch(`https://api.github.com/repos/${REPO}/releases?per_page=40`, { headers: { Accept: "application/vnd.github+json", "User-Agent": "SC2-Overlay" } });
    if (!r.ok) throw new Error("HTTP " + r.status);
    const list = (await r.json()).filter(x => !x.draft && !x.prerelease);
    const best = re => list.filter(x => re.test(x.tag_name) && verOf(x.tag_name))
      .sort((a, b) => verCmp(verOf(b.tag_name), verOf(a.tag_name))).map(x => ({ tag: x.tag_name, url: x.html_url }))[0] || null;
    upd.app = best(/^overlay-v/); upd.fw = best(/^v\d/); upd.checked = Date.now(); upd.error = "";
  } catch (e) { upd.error = e.message; console.warn("update check:", e.message); }
  announce(); buildTray();
  if (manual) {
    const items = updateItems().length - 1; /* the "Updates" submenu is always there */
    if (upd.error) notify("SC2 Overlay", "Could not check for updates: " + upd.error, RELEASES);
    else if (!items) notify("SC2 Overlay", "Everything is up to date" + (upd.fwHave === undefined ? " (connect the controller to check its firmware too)." : "."), null);
  }
}
function updateItems() {
  const items = [], mine = verOf("v" + appVer());
  if (upd.app && verCmp(verOf(upd.app.tag), mine) > 0)
    items.push({ label: `SC2 Overlay ${upd.app.tag.replace("overlay-v", "")} available…`, click: () => shell.openExternal(upd.app.url) });
  if (upd.fw && upd.fwHave !== undefined && (!verOf(upd.fwHave) || verCmp(verOf(upd.fw.tag), verOf(upd.fwHave)) > 0))
    items.push({ label: `Firmware ${upd.fw.tag} available…`, click: () => shell.openExternal(upd.fw.url) });
  items.push({ label: "Updates", submenu: [
    { label: `This app: ${appVer()}`, enabled: false },
    { label: `Controller firmware: ${upd.fwHave === undefined ? "no controller yet" : upd.fwHave || "older than v51.2"}`, enabled: false },
    { label: upd.error ? `Last check failed (${upd.error})` : upd.checked ? `Newest: firmware ${upd.fw ? upd.fw.tag : "?"}, app ${upd.app ? upd.app.tag.replace("overlay-v", "") : "?"}` : "Not checked yet", enabled: false },
    { type: "separator" },
    { label: "Check now", click: () => { conf.told = {}; tell.run = {}; saveConf(); checkUpdates(true); } },
    { label: "Notify me about new versions", type: "checkbox", checked: conf.updates, click: m => { conf.updates = m.checked; saveConf(); if (m.checked) checkUpdates(); } },
    { label: "Open the releases page…", click: () => shell.openExternal(RELEASES) },
  ] });
  return items;
}

function buildTray() {
  if (!tray) return;
  const displays = screen.getAllDisplays(), cur = pickDisplay();
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: "SC2 overlay", enabled: false },
    { type: "separator" },
    { label: editing ? "Done arranging" : "Arrange the overlay…", accelerator: KEY_ARRANGE, click: toggleArrange },
    ...PAGES.map(([f, label]) => ({ label, click: () => openPage(f) })),
    { label: "Recenter the 3D controller", click: recenter },
    { label: "Show the overlay", type: "checkbox", checked: conf.shown, accelerator: KEY_SHOW, click: toggleShown },
    { label: "Screen", submenu: displays.map((d, i) => ({ label: `Screen ${i + 1} (${d.size.width} × ${d.size.height})${d.id === screen.getPrimaryDisplay().id ? ", main" : ""}`,
      type: "radio", checked: d.id === cur.id, click: () => { conf.display = d.id; saveConf(); placeOverlay(); } })) },
    ...(batt ? [{ label: batt.text, enabled: false }] : []),
    ...updateItems(),
    { label: "Use the fast graphics card (restarts)", type: "checkbox", checked: conf.fastGpu,
      click: m => { conf.fastGpu = m.checked; saveConf(); app.relaunch(); app.quit(); } },
    { label: LINUX ? "Start at login" : "Start with Windows", type: "checkbox", checked: getAutostart(),
      click: m => setAutostart(m.checked) },
    { type: "separator" },
    { label: "Quit", click: () => app.quit() },
  ]));
  tray.setToolTip("SC2 overlay" + (editing ? " (arranging)" : "") + (batt ? "\n" + batt.text : ""));
}

/* Command line, for a desktop shortcut or a key binding where global hotkeys are not available (Wayland):
 *   sc2-overlay --arrange | --toggle | --recenter | --settings   (run it while the app is already running) */
function runArgs(argv) {
  let did = false;
  if (argv.includes("--arrange")) { toggleArrange(); did = true; }
  if (argv.includes("--toggle")) { toggleShown(); did = true; }
  if (argv.includes("--recenter")) { recenter(); did = true; }
  if (argv.includes("--settings")) { openRemap(); did = true; }
  return did;
}
const recenter = () => modelWin && modelWin.webContents.executeJavaScript(`window.postMessage("sc26re:recenter", location.origin)`).catch(() => {});
app.on("second-instance", (e, argv) => { if (!runArgs(argv)) openRemap(); });

/* ---------- 1.5: Steam Input community layouts (steam://controllerconfig/<game>/<id>) ----------
 * The remapper downloads one by its number. Steam's file list (ISteamRemoteStorage/GetPublishedFileDetails,
 * no key needed) cannot be asked from a web page (no CORS), so the app asks it: the remapper inside the
 * app through window.sc26reApp.steamConfig, the remapper in the browser on 127.0.0.1:27150 while the app
 * runs (only for this project's pages). Only Steam controller layouts, only from Steam's own file server. */
const STEAMCFG_PORT = 27150;
const STEAMCFG_ORIGINS = ["https://pablo89898991.github.io", ORIGIN];
async function steamConfig(id) {
  id = String(id || "");
  if (!/^\d{6,20}$/.test(id)) throw new Error("not a layout number");
  const body = new URLSearchParams({ itemcount: "1", "publishedfileids[0]": id }).toString();
  const r = await net.fetch("https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/",
    { method: "POST", body, headers: { "Content-Type": "application/x-www-form-urlencoded", "User-Agent": "SC2-Overlay" } });
  if (!r.ok) throw new Error("Steam answered " + r.status);
  const d = (((await r.json()) || {}).response || {}).publishedfiledetails || [];
  const f = d[0];
  if (!f || f.result !== 1) throw new Error("Steam has no layout " + id + " (or it is private)");
  if (Number(f.consumer_app_id) !== 241100 || !/_controller_config\.vdf$/.test(f.filename || "")) throw new Error("item " + id + " is not a Steam Input layout");
  const u = new URL(f.file_url || "about:blank");
  if (u.protocol !== "https:" || !/(^|\.)steamusercontent\.com$/.test(u.hostname)) throw new Error("unexpected file address");
  if (Number(f.file_size) > 2 * 1024 * 1024) throw new Error("the layout file is too big");
  const v = await net.fetch(u.toString(), { headers: { "User-Agent": "SC2-Overlay" } });
  if (!v.ok) throw new Error("download failed: " + v.status);
  const vdf = await v.text();
  if (vdf.length > 2 * 1024 * 1024 || !/"controller_mappings"/.test(vdf)) throw new Error("not a Steam Input layout file");
  return { id, title: String(f.title || ""), filename: String(f.filename || ""), vdf };
}
function steamConfigServer() {
  const http = require("http");
  const srv = http.createServer(async (req, res) => {
    const origin = req.headers.origin || "";
    if (STEAMCFG_ORIGINS.includes(origin)) {
      res.setHeader("Access-Control-Allow-Origin", origin);
      res.setHeader("Vary", "Origin");
      res.setHeader("Access-Control-Allow-Private-Network", "true");
    }
    if (req.method === "OPTIONS") { res.writeHead(204, { "Access-Control-Allow-Methods": "GET" }); res.end(); return; }
    const u = new URL(req.url, "http://127.0.0.1");
    const send = (code, obj) => { res.writeHead(code, { "Content-Type": "application/json" }); res.end(JSON.stringify(obj)); };
    if (req.method !== "GET" || u.pathname !== "/steam-config") return send(404, { error: "not found" });
    if (!STEAMCFG_ORIGINS.includes(origin)) return send(403, { error: "only for the sc26re pages" });
    try { send(200, await steamConfig(u.searchParams.get("id"))); } catch (e) { send(400, { error: e.message }); }
  });
  srv.on("error", e => console.warn("steam-config server:", e.message)); /* the port taken: in-app downloads still work */
  srv.listen(STEAMCFG_PORT, "127.0.0.1");
}

app.whenReady().then(() => {
  protocol.handle("app", req => {
    const p = decodeURIComponent(new URL(req.url).pathname), file = path.normalize(path.join(WEB, p));
    if (!file.startsWith(WEB)) return new Response("no", { status: 403 });
    return net.fetch(pathToFileURL(file).toString());
  });
  /* WebHID: the Steam Controller (Valve's vendor id) is always allowed, without a chooser */
  const ses = session.defaultSession;
  ses.setPermissionCheckHandler((wc, perm) => perm === "hid" || perm === "serial" || perm === "clipboard-sanitized-write");
  ses.setDevicePermissionHandler(d => (d.deviceType === "hid" || d.deviceType === "serial") && Number(d.device.vendorId) === VALVE);
  /* Web Serial: the report meter's flasher talks to the controller's bootloader port (28DE:1005) */
  ses.on("select-serial-port", (e, ports, wc, cb) => {
    e.preventDefault();
    const p = ports.find(x => Number(x.vendorId) === VALVE);
    cb(p ? p.portId : "");
  });
  ses.on("select-hid-device", (e, details, cb) => {
    e.preventDefault();
    /* the controller on the cable (1302) first, else the Puck or Bluetooth; one of its interfaces
     * that has the settings channel when Electron lists them apart */
    const list = details.deviceList.filter(x => x.vendorId === VALVE);
    const d = list.find(x => x.productId === 0x1302) || list[0];
    cb(d ? d.deviceId : "");
  });
  ipcMain.handle("steamConfig", (e, id) => {
    if (!String(e.senderFrame && e.senderFrame.url || "").startsWith(ORIGIN)) throw new Error("not allowed");
    return steamConfig(id);
  });
  steamConfigServer();
  ipcMain.on("arrange", () => toggleArrange());
  ipcMain.on("battery", (e, info) => {
    const b = info && typeof info.text === "string" ? { pct: +info.pct || 0, text: info.text.slice(0, 120), charging: !!info.charging } : null;
    /* low battery: once per discharge, below 15 % */
    if (b && !b.charging && b.pct <= 15 && !lowTold) { lowTold = true; notify("Controller battery low", b.text, null); }
    if (b && (b.charging || b.pct > 20)) lowTold = false;
    const changed = JSON.stringify(b) !== JSON.stringify(batt);
    batt = b; if (changed && tray) buildTray();
  });
  ipcMain.on("firmware", (e, info) => {
    const rel = UPD_TEST ? "v50" : info && typeof info.release === "string" ? info.release.slice(0, 16) : "";
    if (upd.fwHave !== rel) { upd.fwHave = rel; announce(); buildTray(); }
  });
  ipcMain.on("modelRect", (e, r) => {
    if (!r || ![r.x, r.y, r.size].every(Number.isFinite)) return;
    const m = { x: r.x, y: r.y, size: r.size, on: r.on !== false };
    if (JSON.stringify(m) === JSON.stringify(conf.model)) return;
    conf.model = m; saveConf();
    if (modelWin) modelWin.setBounds(modelBounds());
    showModel();
  });
  ipcMain.on("menuShow", (e, on) => { if (overlay && e.sender === overlay.webContents) { menuWanted = !!on; showMenu(); } });
  ipcMain.on("edit", (e, on) => { if (overlay && e.sender === overlay.webContents) setEditing(on); });

  makeOverlay();
  tray = new Tray(nativeImage.createFromPath(path.join(__dirname, "tray.png")));
  tray.on("click", toggleArrange);
  tray.on("balloon-click", () => { if (balloonUrl) shell.openExternal(balloonUrl); });
  buildTray();
  /* register() answers false when the desktop does not give the key (Wayland, or another app has it) */
  if (!globalShortcut.register(KEY_ARRANGE, toggleArrange)) console.warn("hotkey not available: " + KEY_ARRANGE + " (use sc2-overlay --arrange)");
  if (!globalShortcut.register(KEY_SHOW, toggleShown)) console.warn("hotkey not available: " + KEY_SHOW + " (use sc2-overlay --toggle)");
  screen.on("display-removed", () => { placeOverlay(); buildTray(); });
  screen.on("display-added", buildTray);
  screen.on("display-metrics-changed", placeOverlay);
  /* games that grab "topmost" for themselves: take it back now and then */
  setInterval(() => { for (const w of [modelWin, overlay]) if (w && w.isVisible()) w.setAlwaysOnTop(true, "screen-saver"); }, 5000);
  runArgs(process.argv);
  setTimeout(() => { if (conf.updates) checkUpdates(); }, 5000);
  setInterval(() => { if (conf.updates) checkUpdates(); }, 6 * 3600 * 1000);
});

app.on("window-all-closed", e => { /* the tray keeps it running */ });
app.on("will-quit", () => globalShortcut.unregisterAll());
