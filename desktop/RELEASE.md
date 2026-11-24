## SC2 Overlay 1.4 for Windows and Linux

Special thanks to **Rhyn0_Rang3r**, our Discord tester, for making the Linux version and for testing.

**New in 1.4**
- **Smoother 3D controller next to a game.** The 3D controller now has its own small window, apart from the full-screen layer for the radial and touch menus, so the game and the overlay no longer fight over a full-screen redraw.
- **Runs on the fast graphics card.** On laptops with two GPUs the app now uses the fast one. You can switch it off in the tray: *Use the fast graphics card* (the app restarts).

**From 1.3**
- **Battery in the tray.** Hover the tray icon: the controller's battery level, whether it is charging, voltage and temperature. The same line is at the top of the tray menu, and a notification comes once when the battery drops to 15 % while not charging. Needs firmware **v51.4** or newer (the first one that reports the battery); with older firmware the line is simply not there.
- **Lighter 3D controller.** It is drawn only when something changes (a still controller costs about one frame a second instead of the screen's full rate), with a smaller canvas, and the full-screen menu layer is left alone while nothing is on it.
- **Quality setting** (Remapper and overlay settings → Window → Quality): *light* turns edge smoothing off and draws at the window's own resolution, about half the GPU work. Pick it if the overlay stutters next to a game.

**From 1.2: update notifications.** At start and every 6 hours the app checks this project's releases and tells you when a new **SC2 Overlay** or a new **firmware** is out (tray → *Updates*). Nothing is downloaded or installed by itself.

Firmware for this app: **[sc26re v51.5](https://github.com/pablo89898991/sc26re-1khz/releases/tag/v51.5)**.

A small Windows and Linux app for the **sc26re** firmware (Steam Controller 2026, v51.5 recommended):

- **See-through overlay over your game:** radial menus and touch menus (3 × 3 grids) with the name of each slice and the chosen one lit, the profile's name when it changes, and the 3D controller. It is always on top, and your clicks go through to the game.
- **Arrange it on screen:** Ctrl+Shift+F10 (or the tray icon) shows sample menus and the 3D controller's area. Drag them where you want them, use the mouse wheel to resize, and press Esc when done. Ctrl+Shift+F9 shows / hides the overlay.
- **Every tool inside, offline:** the remapper (its Overlay tab sets the overlay up live), the report meter with the flasher, the 3D overlay page, the Puck patcher. No browser and no internet needed.
- Tray menu: screen choice, start with Windows, update check, quit.

**Use:** download `SC2-Overlay.exe` and run it (no install). Windows SmartScreen may warn because the app is not signed: *More info → Run anyway*.

**Limits:** shows over games in a window or borderless full screen, not over exclusive full screen (Windows does not allow it). The on-screen menus need sc26re v51.

## Linux

The Linux version was made by **Rhyn0_Rang3r**. Thank you!

- **Ubuntu / Debian:** `SC2-Overlay-1.4.0.deb`. Install it with `sudo apt install ./SC2-Overlay-1.4.0.deb`, then unplug and replug the controller once. The package also installs the controller access rules (udev).
- **Other distributions:** `SC2-Overlay-1.4.0.AppImage`. Make it executable and run it. On Ubuntu 24.04+ it may need `--no-sandbox`.
- **Wayland:** global hotkeys don't reach apps, so bind these commands to keys (the installer script does it for GNOME): `sc2-overlay --arrange`, `sc2-overlay --toggle`, `sc2-overlay --recenter`, `sc2-overlay --settings`.
- **Arranging:** leave with the blue **Done arranging** button or a right-click.
- **Building from source:** use `install-linux.sh` in the repository. Details are in `desktop/LINUX.md`.
