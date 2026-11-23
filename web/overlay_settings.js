/* Overlay settings: one list for the overlay's own panel (overlay.html, H) and the remapper's
 * Overlay tab (remap.html), so both show the same controls. Values live in localStorage:
 * the overlay page on its own under "sc26re_overlay2", the remapper's overlay (in the tab and in
 * the window on top) under "sc26re_overlay_tab"; a change in one document reaches the others
 * through the storage event. */
(function () {
  const POS = [["auto", "the input's side"], ["center", "the centre"], ["custom", "where I put it"]];
  const G = [
    { g: "menu", name: "On-screen menus", rows: [
      [{ t: "check", id: "menuOn", label: "on-screen menus", def: true, title: "radial menus and touch menus (3 x 3 grids) drawn with what each slice does, while a pad is touched or a stick pushed (v51 test 84 or newer)" }],
      [{ t: "sub", label: "Radial menu" }],
      [{ t: "select", id: "radPos", label: "Place", opts: POS, def: "auto" }],
      [{ t: "range", id: "radX", label: "X", min: 0, max: 100, def: 50, unit: "%", title: "0 = left edge, 100 = right edge" }],
      [{ t: "range", id: "radY", label: "Y", min: 0, max: 100, def: 50, unit: "%", title: "0 = top, 100 = bottom" }],
      [{ t: "range", id: "radSize", label: "Size", min: 20, max: 250, def: 100, unit: "%" }],
      [{ t: "sub", label: "Touch menu (3 × 3 grid)" }],
      [{ t: "select", id: "gridPos", label: "Place", opts: POS, def: "auto" }],
      [{ t: "range", id: "gridX", label: "X", min: 0, max: 100, def: 50, unit: "%", title: "0 = left edge, 100 = right edge" }],
      [{ t: "range", id: "gridY", label: "Y", min: 0, max: 100, def: 50, unit: "%", title: "0 = top, 100 = bottom" }],
      [{ t: "range", id: "gridSize", label: "Size", min: 20, max: 250, def: 100, unit: "%" }],
      [{ t: "sub", label: "Look" }],
      [{ t: "color", id: "menuCol", label: "Chosen slice", def: "#1a9fff" }],
      [{ t: "range", id: "menuAlpha", label: "Opacity", min: 20, max: 100, def: 94, unit: "%" }],
      [{ t: "sub", label: "Profile changes" }],
      [{ t: "check", id: "toastOn", label: "show the profile's name when it changes", def: true, title: "for a moment, also when an action layer comes on" }],
      [{ t: "range", id: "toastY", label: "Y", min: 0, max: 100, def: 8, unit: "%" }],
    ] },
    { g: "model", name: "3D controller", rows: [
      [{ t: "check", id: "modelOn", label: "3D controller", def: true }],
      [{ t: "range", id: "modelX", label: "X", min: 0, max: 100, def: 50, unit: "%", title: "0 = left edge, 100 = right edge" }],
      [{ t: "range", id: "modelY", label: "Y", min: 0, max: 100, def: 50, unit: "%", title: "0 = top, 100 = bottom" }],
      [{ t: "range", id: "modelSize", label: "Area", min: 10, max: 100, def: 100, unit: "%", title: "the part of the window the model is drawn in" }],
      [{ t: "range", id: "zoom", label: "Size", min: 40, max: 160, def: 100, unit: "%", title: "the model inside its area" }],
      [{ t: "select", id: "src", label: "Shows", opts: [["raw", "the physical controller (native, also when remapped)"], ["steam", "what Steam receives"]], def: "raw" }],
      [{ t: "select", id: "mdl", label: "Model", opts: [["valve", "Valve’s official model (exact shape)"], ["detail", "detailed, textured"]], def: "valve" }],
      [{ t: "color", id: "hiCol", label: "Pressed", def: "#1a9fff" }, { t: "color", id: "touchCol", label: "Touched", def: "#5fe39a" }],
      [{ t: "range", id: "tilt", label: "View tilt", min: -30, max: 90, def: 20 }],
      [{ t: "check", id: "xray", label: "see-through body (back buttons and triggers stay visible)", def: false }],
      [{ t: "label", label: "Pop out" }, { t: "check", id: "popTrig", label: "triggers", def: true }, { t: "check", id: "popBump", label: "bumpers", def: true }, { t: "check", id: "popBack", label: "back buttons", def: true }],
      [{ t: "check", id: "gripShow", label: "grip sense shown through the body", def: true }],
      [{ t: "check", id: "useGyro", label: "gyro moves it", def: true }, { t: "button", id: "recenter", label: "Recenter (R)" }],
      [{ t: "range", id: "sens", label: "Gyro speed", min: 10, max: 300, def: 100, out: "sensV", title: "how far the model turns for a turn of the controller (1.00x = the same)" }],
      [{ t: "check", id: "level", label: "correct tilt (gravity)", def: false, title: "pulls the model level with gravity (the accelerometer); the time is how long it takes to close most of a tilt error" }, { t: "range", id: "levelK", min: 1, max: 100, def: 50, out: "levelV" }],
      [{ t: "check", id: "yawFix", label: "correct heading drift", def: false, title: "the heading drifts slowly (nothing measures it): pulls it back to where Recenter put it; the time is how long it takes" }, { t: "range", id: "yawK", min: 1, max: 100, def: 30, out: "yawV" }],
      [{ t: "check", id: "imuOn", label: "turn gyro on without Steam", def: false }],
      [{ t: "label", label: "Invert" }, { t: "check", id: "invP", label: "pitch", def: false }, { t: "check", id: "invY", label: "yaw", def: false }, { t: "check", id: "invR", label: "roll", def: false }],
    ] },
    { g: "look", name: "Window", rows: [
      [{ t: "select", id: "bg", label: "Background", opts: [["#00ff00", "green (chroma key)"], ["#ff00ff", "magenta (chroma key)"], ["#0d1320", "dark"], ["#ffffff", "white"], ["custom", "my colour"], ["transparent", "transparent"]], def: "#00ff00" }, { t: "color", id: "bgCol", def: "#202020", title: "the colour for “my colour”" }],
      [{ t: "select", id: "fps", label: "Frame rate", opts: [["0", "as the screen"], ["30", "30"], ["60", "60"], ["120", "120"], ["144", "144"], ["240", "240"]], def: "0", out: "fpsV" }],
    ] },
  ];
  const all = []; for (const g of G) for (const r of g.rows) for (const it of r) if (it.id && it.t !== "button") all.push(it);
  const esc = s => String(s).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
  function item(it, p) {
    const id = p + it.id, tt = it.title ? ` title="${esc(it.title)}"` : "";
    switch (it.t) {
      case "sub": return `<b class="ovsub">${it.label}</b>`;
      case "label": return `<label${tt}>${it.label}</label>`;
      case "check": return `<label${tt}><input type="checkbox" id="${id}"${it.def ? " checked" : ""}> ${it.label}</label>`;
      case "button": return `<button id="${id}">${it.label}</button>`;
      case "color": return (it.label ? `<label${tt}>${it.label}</label>` : "") + `<input type="color" id="${id}" value="${it.def}"${tt}>`;
      case "select": return (it.label ? `<label${tt}>${it.label}</label>` : "") + `<select id="${id}"${tt}>${it.opts.map(([v, n]) => `<option value="${esc(v)}"${v === it.def ? " selected" : ""}>${n}</option>`).join("")}</select>` + (it.out ? `<span id="${p + it.out}" class="note"></span>` : "");
      case "range": return (it.label ? `<label${tt}>${it.label}</label>` : "") + `<input type="range" id="${id}" min="${it.min}" max="${it.max}" value="${it.def}"${it.step ? ` step="${it.step}"` : ""}${tt}>` +
        (it.out ? `<span id="${p + it.out}" class="note"></span>` : it.unit !== undefined || p ? `<span class="note ovval" data-for="${id}" data-unit="${it.unit || ""}"></span>` : "");
    }
    return "";
  }
  /* rows of one group */
  function rows(g, p = "") { return G.find(x => x.g === g).rows.map(r => `<div class="row">${r.map(it => item(it, p)).join("")}</div>`).join(""); }
  /* the number next to a slider */
  function showVals(root) { root.querySelectorAll(".ovval").forEach(s => { const e = root.querySelector("#" + s.dataset.for); if (e) s.textContent = e.value + s.dataset.unit; }); }
  function read(key) { try { return JSON.parse(localStorage.getItem(key) || "{}"); } catch (e) { return {}; } }
  /* values into the inputs (p = id prefix); returns the ids that changed */
  function apply(root, o, p = "") {
    const ch = [];
    for (const it of all) { if (!(it.id in o)) continue; const e = root.querySelector("#" + p + it.id); if (!e) continue;
      if (e.type === "checkbox") { if (e.checked !== !!o[it.id]) { e.checked = !!o[it.id]; ch.push(e); } }
      else if (e.value !== String(o[it.id])) { e.value = o[it.id]; ch.push(e); } }
    showVals(root); return ch;
  }
  function collect(root, p = "") { const o = {}; for (const it of all) { const e = root.querySelector("#" + p + it.id); if (e) o[it.id] = e.type === "checkbox" ? e.checked : e.value; } return o; }
  window.OVS = { groups: G, all, rows, apply, collect, read, showVals, ids: all.map(x => x.id), TAB_KEY: "sc26re_overlay_tab", PAGE_KEY: "sc26re_overlay2",
    /* defaults that differ inside the remapper: a dark background */
    tabDefaults: { bg: "#0d1320" },
    /* the overlay app over the whole screen: the 3D controller small, bottom right */
    appDefaults: { modelSize: 30, modelX: 85, modelY: 78 } };
})();
