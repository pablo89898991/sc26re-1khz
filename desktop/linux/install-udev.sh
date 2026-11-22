#!/bin/sh
# One-time setup so the app can open the controller / Puck (WebHID) and the flasher's serial port.
# Run once:  sudo ./install-udev.sh   then unplug and replug the controller.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
install -m 644 "$here/70-sc26re.rules" /etc/udev/rules.d/70-sc26re.rules
udevadm control --reload-rules
udevadm trigger
echo "Installed /etc/udev/rules.d/70-sc26re.rules. Unplug and replug the controller."
