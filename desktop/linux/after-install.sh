#!/bin/sh
# .deb post-install. Replaces electron-builder's default one, which leaves Chromium's sandbox helper
# without the setuid bit on Ubuntu 24.04+ (the app then refuses to start).
APP="/opt/SC2-Overlay"
EXE="sc2-overlay"

# command on the PATH
if type update-alternatives >/dev/null 2>&1; then
  if [ -L "/usr/bin/$EXE" ] && [ -e "/usr/bin/$EXE" ] && [ "$(readlink "/usr/bin/$EXE")" != "/etc/alternatives/$EXE" ]; then
    rm -f "/usr/bin/$EXE"
  fi
  update-alternatives --install "/usr/bin/$EXE" "$EXE" "$APP/$EXE" 100 || ln -sf "$APP/$EXE" "/usr/bin/$EXE"
else
  ln -sf "$APP/$EXE" "/usr/bin/$EXE"
fi

# sandbox helper: always setuid root
chown root:root "$APP/chrome-sandbox" 2>/dev/null || true
chmod 4755 "$APP/chrome-sandbox" || true

# controller access (WebHID and the flasher's serial port)
if [ -f "$APP/resources/70-sc26re.rules" ]; then
  cp "$APP/resources/70-sc26re.rules" /etc/udev/rules.d/70-sc26re.rules
  if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger || true
  fi
fi

if command -v update-mime-database >/dev/null 2>&1; then update-mime-database /usr/share/mime || true; fi
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database /usr/share/applications || true; fi
exit 0
