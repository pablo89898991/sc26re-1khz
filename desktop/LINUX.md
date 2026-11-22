# SC2 Overlay on Linux

The same Electron app as the Windows build, with a Linux target. Tested on Ubuntu (GNOME, Wayland).

## Quick install (Ubuntu / Debian)

From the project folder (the one with `desktop/` and `web/`), as your normal user:

    ./install-linux.sh

It installs Node 20 if needed, builds the app, installs the `.deb`, sets up the controller access rules
and, if you say yes, adds the GNOME hotkeys. `./install-linux.sh -y` asks nothing; `--no-keys` skips the
hotkeys; `./install-linux.sh --uninstall` removes everything it added.

Then start it with `sc2-overlay` (or from the app menu) and replug the controller once.

## By hand

    cd desktop
    ./build-linux.sh                                   # makes dist/SC2-Overlay-<ver>.deb and .AppImage
    sudo apt install ./dist/SC2-Overlay-*.deb          # the installer sets the sandbox and udev rules

Try it without packaging: `npm install && npm start -- --no-sandbox`.

The product name is `SC2-Overlay` on Linux (no space): Chromium's sandbox launcher cannot start a program
under a folder called `/opt/SC2 Overlay`.

## Hotkeys on Wayland

Global hotkeys do not reach an app on a Wayland session, so Ctrl+Shift+F10 and F9 do nothing inside the
app. Let the desktop run the commands instead (the installer adds these in GNOME):

    sc2-overlay --arrange      # Ctrl+Shift+F10: start / end arrange mode
    sc2-overlay --toggle       # Ctrl+Shift+F9:  show / hide the overlay
    sc2-overlay --recenter     # recenter the 3D controller
    sc2-overlay --settings     # open the remapper

They talk to the copy that is already running. On other desktops, bind them in the keyboard settings.

## Arrange mode

Drag the menus and the 3D controller, scroll to resize. Leave with the blue **Done arranging** button, a
right-click, the tray menu, or the arrange hotkey again. Esc does not work on Linux: the overlay window
cannot take keyboard focus (that is what keeps it from stealing focus from your game).

## Notes

- The app runs through X11 (XWayland on a Wayland session) so always-on-top, fixed position and click-through
  work. `SC2_OVERLAY_WAYLAND=1` runs native Wayland instead (not recommended).
- Like on Windows, it shows over games in a window or borderless full screen, not exclusive full screen.
- "Start at login" in the tray menu writes `~/.config/autostart/sc2-overlay.desktop`.
- Controller access: `desktop/linux/70-sc26re.rules` (installed by the `.deb`) gives your user access to
  Valve devices (28de) over hidraw, USB and the flasher's serial port. Steam's own rules already cover HID.
- AppImage on Ubuntu 24.04+: the sandbox may refuse to start; run it with `--no-sandbox`, or use the `.deb`.
- The tray icon on GNOME needs the AppIndicator extension (included with Ubuntu).
