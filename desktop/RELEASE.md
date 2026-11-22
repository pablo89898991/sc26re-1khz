## SC2 Overlay 1.2 for Windows and Linux

Special thanks to **Rhyn0_Rang3r**, our Discord tester, for making the Linux version and for testing.

**New in 1.2: update notifications.** At start and every 6 hours the app checks this project's releases and shows a notification when a new **SC2 Overlay** or a new **firmware** is out (once per version; a click opens the download page). The tray's *Updates* menu shows the app's version, the connected controller's firmware, *Check now* and an on/off switch. Nothing is downloaded or installed by itself. The firmware check needs **[sc26re v51.2](https://github.com/pablo89898991/sc26re-1khz/releases/tag/v51.2)** or newer on the controller (older firmware shows as "older than v51.2").

Firmware for this app: **[sc26re v51.2](https://github.com/pablo89898991/sc26re-1khz/releases/tag/v51.2)**.

A small Windows and Linux app for the **sc26re** firmware (Steam Controller 2026, v51.2 recommended):

- **See-through overlay over your game:** radial menus and touch menus (3 × 3 grids) with the name of each slice and the chosen one lit, the profile's name when it changes, and the 3D controller. It is always on top, and your clicks go through to the game.
- **Arrange it on screen:** Ctrl+Shift+F10 (or the tray icon) shows sample menus and the 3D controller's area. Drag them where you want them, use the mouse wheel to resize, and press Esc when done. Ctrl+Shift+F9 shows / hides the overlay.
- **Every tool inside, offline:** the remapper (its Overlay tab sets the overlay up live), the report meter with the flasher, the 3D overlay page, the Puck patcher. No browser and no internet needed.
- Tray menu: screen choice, start with Windows, update check, quit.

**Use:** download `SC2-Overlay.exe` and run it (no install). Windows SmartScreen may warn because the app is not signed: *More info → Run anyway*.

**Limits:** shows over games in a window or borderless full screen, not over exclusive full screen (Windows does not allow it). The on-screen menus need sc26re v51.

## Linux

The Linux version was made by **Rhyn0_Rang3r**. Thank you!

- **Ubuntu / Debian:** `SC2-Overlay-1.2.0.deb`. Install it with `sudo apt install ./SC2-Overlay-1.2.0.deb`, then unplug and replug the controller once. The package also installs the controller access rules (udev).
- **Other distributions:** `SC2-Overlay-1.2.0.AppImage`. Make it executable and run it. On Ubuntu 24.04+ it may need `--no-sandbox`.
- **Wayland:** global hotkeys don't reach apps, so bind these commands to keys (the installer script does it for GNOME): `sc2-overlay --arrange`, `sc2-overlay --toggle`, `sc2-overlay --recenter`, `sc2-overlay --settings`.
- **Arranging:** leave with the blue **Done arranging** button or a right-click.
- **Building from source:** use `install-linux.sh` in the repository. Details are in `desktop/LINUX.md`.
