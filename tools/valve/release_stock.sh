#!/usr/bin/env bash
# Patch the newest Valve controller + Puck firmware with the web-patcher engine and publish a release.
# usage: release_stock.sh <workdir>   (needs gh, node, python3; GH_TOKEN in env)
set -euo pipefail
W=${1:?workdir}
mkdir -p "$W"
RAW=${VALVE_FW_RAW:-https://raw.githubusercontent.com/OpenSteamController/Ibex-Firmware/main}
curl -fsSL "$RAW/index.json" -o "$W/index.json"
read -r CTRL PUCK < <(python3 - "$W/index.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
c = max(d["controller"].values(), key=lambda m: m["version_unix"])["version_hex"]
p = max(d["puck"].values(), key=lambda m: m["version_unix"])["version_hex"]
print(c, p)
PY
)
TAG="stock-1k-${CTRL}-${PUCK}"
if gh release view "$TAG" >/dev/null 2>&1; then
  echo "release $TAG already exists"; exit 0
fi
curl -fsSL "$RAW/Controller/IBEX_FW_${CTRL}.fw" -o "$W/IBEX_FW_${CTRL}.fw"
curl -fsSL "$RAW/Puck/PROTEUS_FW_${PUCK}.fw" -o "$W/PROTEUS_FW_${PUCK}.fw"

NOTES="$W/notes.md"
{
  echo "Valve's stock firmware, automatically patched by the SC2 Patcher engine (no sc26re code)."
  echo
  echo "- Controller: \`IBEX_FW_${CTRL}\`"
  echo "- Puck: \`PROTEUS_FW_${PUCK}\`"
  echo
  echo "These get every Valve fix the day it ships. For the highest rates (~980 reports/s over USB, ~828 over the Puck) use the sc26re build instead."
  echo
  echo "> Built automatically and **not tested on hardware**. Patch sites are located by code structure and the patcher refuses when they don't match, but keep a known-good firmware at hand (the bootloader always lets you reflash)."
  echo
} > "$NOTES"

ASSETS=()
CTRL_OK=""
for preset in balanced maxrep usb1k; do
  OUT="$W/IBEX_FW_${CTRL}_${preset}.fw"
  if node tools/stock_patch.js "$W/IBEX_FW_${CTRL}.fw" "$OUT" "$preset" > "$W/ctrl_${preset}.log" 2>&1; then
    CTRL_OK=$preset
    ASSETS+=("$OUT")
    { echo "### Controller — preset \`$preset\`"; echo '```'; cat "$W/ctrl_${preset}.log"; echo '```'; } >> "$NOTES"
    break
  fi
  { echo "<details><summary>Controller preset \`$preset\` not applicable</summary>"; echo; echo '```'; cat "$W/ctrl_${preset}.log"; echo '```'; echo "</details>"; echo; } >> "$NOTES"
done
[ -n "$CTRL_OK" ] || echo "**No controller patch preset fits this Valve version — the patcher needs updating.**" >> "$NOTES"

for preset in pollmax poll05; do
  OUT="$W/PROTEUS_FW_${PUCK}_${preset}.fw"
  if node tools/stock_patch.js "$W/PROTEUS_FW_${PUCK}.fw" "$OUT" "$preset" > "$W/puck_${preset}.log" 2>&1; then
    ASSETS+=("$OUT")
    { echo "### Puck — \`$preset\`"; echo '```'; cat "$W/puck_${preset}.log"; echo '```'; } >> "$NOTES"
  else
    { echo "### Puck — \`$preset\` FAILED"; echo '```'; cat "$W/puck_${preset}.log"; echo '```'; } >> "$NOTES"
  fi
done

if [ ${#ASSETS[@]} -eq 0 ]; then
  echo "nothing could be patched"; cat "$NOTES"; exit 1
fi
gh release create "$TAG" "${ASSETS[@]}" --title "Stock ${CTRL} + Puck ${PUCK} (1 kHz patched)" --notes-file "$NOTES"
echo "released $TAG"
