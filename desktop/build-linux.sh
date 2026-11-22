#!/bin/sh
# Build the Linux packages (.deb and AppImage) into desktop/dist/. Needs Node 18+ and internet for npm.
# The product name has no space: Chromium's sandbox launcher cannot start a program under "/opt/SC2 Overlay".
set -e
cd "$(dirname "$0")"
npm install --no-audit --no-fund
npx electron-builder --linux deb AppImage --x64 --publish never -c.productName=SC2-Overlay
ls -1 dist/*.deb dist/*.AppImage
