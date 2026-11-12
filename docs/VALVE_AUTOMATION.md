# Keeping up with Valve firmware automatically

`.github/workflows/valve-watch.yml` runs every day (and on demand from the Actions tab):

1. Reads `index.json` of [OpenSteamController/Ibex-Firmware](https://github.com/OpenSteamController/Ibex-Firmware)
   and compares it with `valve/state.json`.
2. **New controller firmware** → an issue labelled `valve-update` with a change report against the last
   version ported into sc26re (`controller_baseline`): settings-table changes, new/removed Steam feature
   commands, new log strings, changed and new functions.
   Settings-table changes (defaults, ranges, new entries) are applied automatically on a branch
   `valve/<version>-settings` and opened as a pull request. Everything else has to be read from the binary
   and re-implemented by hand; after that, bump `controller_baseline` / `controller_baseline_unix`.
3. **New controller or Puck firmware** → a release `stock-1k-<controller>-<puck>` with Valve's stock files
   patched by the SC2 Patcher engine (`tools/stock_patch.js`, same code as `web/patcher.html`):
   controller preset `balanced`, falling back to `maxrep`/`usb1k` when a patch site is missing in the new
   version; Puck `pollmax` and `poll05`.

Local use:

```
python3 tools/valve/valve_watch.py report OLD.fw NEW.fw --registry app/src/ibex_settings_registry.c --objdump arm-zephyr-eabi-objdump
python3 tools/valve/valve_watch.py apply-settings OLD.fw NEW.fw --registry app/src/ibex_settings_registry.c --header app/src/ibex_settings_registry.h
node tools/stock_patch.js IBEX_FW_x.fw out.fw maxrep
```

For pull requests to be opened automatically enable *Settings → Actions → General → Allow GitHub Actions
to create and approve pull requests*; otherwise the issue links the branch instead.
