# 1 kHz fork notes

Changes on top of upstream sc26re (branch `imu-1khz`):

| Area | Change | Result (measured, SC2 Report Meter) |
|---|---|---|
| IMU | LSM6DSV16X really runs in high-accuracy ODR (HAODR_SEL + OP_MODE), gyro 1000 Hz / accel 500 Hz; lean FIFO (no TS words, accel 240, SFLP 120, watermark 1) | batch period 1047 -> 998 us |
| USB | 1 ms report loop, gyro sample wakes the loop, single coalesced state slot | ~980 reports/s, ~954 fresh gyro/s |
| Analog | supply kept on and 1 SAADC round on fast links (USB, Puck) | scan time cut ~1.2 ms |
| Puck link | 1 ms loop, stale not-yet-sent ACK payloads replaced by the newest report | controller builds ~1000/s |
| Puck bonds | falls back to bonds stored by the original firmware (bootloader erases CFW storage on every flash) | Puck connects after flashing |
| Trackpads | Olympus frame period 4 -> 1 ms, header+payload in one I2C read, i2c1 at 800 kHz | touched ~250 -> ~305 frames/s (controller's measurement limit) |
| Report loop | fixed 1 ms deadline on fast links | ~930 reports/s even with the IMU off |
| Sticks | forced stick-touch threshold 720 / 576 (Kconfig) | |
| LED | RGBW scaled by Steam's `led_user_brightness` (setting 45) | |

Wireless rate is capped by the official Puck (Proteus) firmware: its connected
slot loop waits 99 ticks (~3 ms) before each poll. `tools/proteus_poll_patch.py`
patches `PROTEUS_FW_6AC6869B.fw` (not included; take it from Steam's
HardwareUpdate folder):

| Puck variant | Reports/s with this firmware |
|---|---|
| stock | ~240 |
| poll1ms | ~516 |
| poll05 | ~680 |
| pollmax | ~828 |

Flash the Puck with `flash.py enter-bl --serial <puck>` then
`flash.py bl-flash --firmware <file> --serial <puck>`.

Note: commit "IMU: keep FIFO timestamp words (gyro read 0 without them on
hardware)" is based on a mis-measurement (Steam had the IMU switched off);
timestamp words were dropped again in the following commits.

## Ported from Valve OFW 6AE90BC5 (public beta, 2026-11-02)

- Trackpad click thresholds: press 500 → 780, release 400 → 680 (settings 30/31 defaults).
- New settings-registry entry 86 (default 0, range 0..1; meaning not yet known).
- "Continue wireless operation (USB/PUCK)": a reboot while the radio is the active link
  (personality switch, Steam-commanded reboot, Steam press while on a charger) sets the
  same retained magic as OFW (0xd5cee019 @ 0x2001ffd4), so after the restart the controller
  goes straight back to wireless instead of charge-only / USB mode when a charger or the
  puck cradle is attached. If a PC enumerates USB the controller still switches to USB.
