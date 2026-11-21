// SC2 overlay for Windows: the overlay page (web/overlay.html) in a see-through window over the
// whole screen, always on top and letting every click through to the game. The tray icon
// arranges it, opens the remapper (its Overlay tab sets the overlay up live), picks the screen.
const { app, BrowserWindow, Tray, Menu, screen, session, protocol, net, globalShortcut, ipcMain, nativeImage } = require("electron");
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

let conf = { display: null, shown: true };
try { conf = Object.assign(conf, JSON.parse(fs.readFileSync(CONF, "utf8"))); } catch (e) {}
const saveConf = () => { try { fs.writeFileSync(CONF, JSON.stringify(conf)); } catch (e) {} };

/* the pages come from a secure origin of their own (WebHID needs one; localStorage is shared, so the
 * remapper window's Overlay tab and the overlay window see each other's settings) */
protocol.registerSchemesAsPrivileged([{ scheme: "app", privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true } }]);

if (!app.requestSingleInstanceLock()) { app.quit(); }
app.commandLine.appendSwitch("disable-renderer-backgrounding");

let overlay = null, remap = null, tray = null, editing = false;

function pickDisplay() {
  const all = screen.getAllDisplays();
  return all.find(d => d.id === conf.display) || screen.getPrimaryDisplay();
}

function makeOverlay() {
  const d = pickDisplay();
  overlay = new BrowserWindow({
    x: d.bounds.x, y: d.bounds.y, width: d.bounds.width, height: d.bounds.height,
    transparent: true, backgroundColor: "#00000000", frame: false, hasShadow: false, resizable: false, movable: false,
    skipTaskbar: true, focusable: false, alwaysOnTop: true, fullscreenable: false, show: false, icon: ICON,
    webPreferences: { preload: path.join(__dirname, "preload.js"), backgroundThrottling: false },
  });
  overlay.setAlwaysOnTop(true, "screen-saver");
  overlay.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
  overlay.setIgnoreMouseEvents(true);
  overlay.loadURL(ORIGIN + "/overlay.html?pip=1&app=1");
  overlay.once("ready-to-show", () => { if (conf.shown) overlay.showInactive(); });
  overlay.on("closed", () => { overlay = null; });
}

function placeOverlay() {
  if (!overlay) return;
  const d = pickDisplay(); overlay.setBounds(d.bounds);
}

function setEditing(on) {
  editing = on;
  if (!overlay) return;
  overlay.setIgnoreMouseEvents(!on);
  overlay.setFocusable(on);
  if (on) { if (!overlay.isVisible()) overlay.showInactive(); overlay.focus(); }
  overlay.setAlwaysOnTop(true, "screen-saver");
  buildTray();
}

function toggleArrange() {
  if (!overlay) return;
  overlay.webContents.executeJavaScript(`window.__ovEdit && window.__ovEdit(${!editing})`).catch(() => {});
}

function toggleShown() {
  conf.shown = !conf.shown; saveConf();
  if (overlay) { if (conf.shown) overlay.showInactive(); else { if (editing) toggleArrange(); overlay.hide(); } }
  buildTray();
}

function openRemap() {
  if (remap) { remap.show(); remap.focus(); return; }
  remap = new BrowserWindow({ width: 1320, height: 940, icon: ICON, title: "SC2 Remapper", autoHideMenuBar: true,
    webPreferences: { preload: path.join(__dirname, "preload.js") } });
  remap.loadURL(ORIGIN + "/remap.html");
  /* links that leave the app (help, licences) open in the normal browser */
  remap.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith(ORIGIN)) return { action: "allow", overrideBrowserWindowOptions: { autoHideMenuBar: true, icon: ICON } };
    require("electron").shell.openExternal(url); return { action: "deny" };
  });
  remap.on("closed", () => { remap = null; });
}

function buildTray() {
  if (!tray) return;
  const displays = screen.getAllDisplays(), cur = pickDisplay();
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: "SC2 overlay", enabled: false },
    { type: "separator" },
    { label: editing ? "Done arranging" : "Arrange the overlay…", accelerator: KEY_ARRANGE, click: toggleArrange },
    { label: "Remapper and overlay settings…", click: openRemap },
    { label: "Recenter the 3D controller", click: () => overlay && overlay.webContents.executeJavaScript(`window.postMessage("sc26re:recenter", location.origin)`).catch(() => {}) },
    { label: "Show the overlay", type: "checkbox", checked: conf.shown, accelerator: KEY_SHOW, click: toggleShown },
    { label: "Screen", submenu: displays.map((d, i) => ({ label: `Screen ${i + 1} (${d.size.width} × ${d.size.height})${d.id === screen.getPrimaryDisplay().id ? ", main" : ""}`,
      type: "radio", checked: d.id === cur.id, click: () => { conf.display = d.id; saveConf(); placeOverlay(); } })) },
    { label: "Start with Windows", type: "checkbox", checked: app.getLoginItemSettings().openAtLogin,
      click: m => app.setLoginItemSettings({ openAtLogin: m.checked }) },
    { type: "separator" },
    { label: "Quit", click: () => app.quit() },
  ]));
  tray.setToolTip("SC2 overlay" + (editing ? " (arranging)" : ""));
}

app.on("second-instance", () => openRemap());

app.whenReady().then(() => {
  protocol.handle("app", req => {
    const p = decodeURIComponent(new URL(req.url).pathname), file = path.normalize(path.join(WEB, p));
    if (!file.startsWith(WEB)) return new Response("no", { status: 403 });
    return net.fetch(pathToFileURL(file).toString());
  });
  /* WebHID: the Steam Controller (Valve's vendor id) is always allowed, without a chooser */
  const ses = session.defaultSession;
  ses.setPermissionCheckHandler((wc, perm) => perm === "hid" || perm === "clipboard-sanitized-write");
  ses.setDevicePermissionHandler(d => d.deviceType === "hid" && d.device.vendorId === VALVE);
  ses.on("select-hid-device", (e, details, cb) => {
    e.preventDefault();
    const d = details.deviceList.find(x => x.vendorId === VALVE);
    cb(d ? d.deviceId : "");
  });
  ipcMain.on("arrange", () => toggleArrange());
  ipcMain.on("edit", (e, on) => { if (overlay && e.sender === overlay.webContents) setEditing(on); });

  makeOverlay();
  tray = new Tray(nativeImage.createFromPath(path.join(__dirname, "tray.png")));
  tray.on("click", toggleArrange);
  buildTray();
  globalShortcut.register(KEY_ARRANGE, toggleArrange);
  globalShortcut.register(KEY_SHOW, toggleShown);
  screen.on("display-removed", () => { placeOverlay(); buildTray(); });
  screen.on("display-added", buildTray);
  screen.on("display-metrics-changed", placeOverlay);
  /* games that grab "topmost" for themselves: take it back now and then */
  setInterval(() => { if (overlay && conf.shown) overlay.setAlwaysOnTop(true, "screen-saver"); }, 5000);
  if (process.argv.includes("--settings")) openRemap();
});

app.on("window-all-closed", e => { /* the tray keeps it running */ });
app.on("will-quit", () => globalShortcut.unregisterAll());
