// The pages (overlay.html, remap.html) see window.sc26reApp only inside the Windows overlay app.
const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld("sc26reApp", {
  /* the overlay's arrange mode: the see-through window takes the mouse only while it is on */
  edit: on => ipcRenderer.send("edit", !!on),
  /* the remapper's Arrange button: arranges the overlay over the screen */
  arrange: () => ipcRenderer.send("arrange"),
  /* the overlay page tells which firmware the connected controller runs (update check) */
  firmware: info => ipcRenderer.send("firmware", info),
  /* controller battery for the tray icon's tooltip (null: none reported) */
  battery: info => ipcRenderer.send("battery", info),
});
