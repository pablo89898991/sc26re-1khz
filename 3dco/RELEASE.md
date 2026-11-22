## 3D Controller Overlay — sc26re edition

[3D Controller Overlay](https://github.com/ceski-1/3d-controller-overlay) by ceski, adapted for the **sc26re v51+** firmware (Steam Controller 2026). It is MIT licensed, and `license.txt` is in the zip.

- **Shows every physical input, even with the remapper on:** buttons, back buttons, triggers, sticks, both trackpads with pressure, and stick and grip touch. The overlay reads them from the controller itself, so it doesn't matter what the remapper sends to the PC (keys, mouse or nothing).
- **Exact gyro:** the 3D model turns with the controller's own 1 kHz gyro integration. It keeps working while the remapper's gyro mouse takes the gyro away from the PC.
- Works over the USB cable, the Puck and Bluetooth. Stock firmware and other controllers work as before.

**Use:**
1. Unzip `3dco-sc26re-windows.zip` and run `3dco.exe`.
2. In the settings window, add a window with the **Steam Controller** model.
3. In the *Controller* section, you should see: "sc26re: physical inputs and gyro, N reads/s".
4. To see the gyro, tick *Gyro → Enable Gyro*.

For OBS, capture the controller window. The overlay's background colour is in its settings.
