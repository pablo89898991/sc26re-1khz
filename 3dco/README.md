# 3D Controller Overlay — sc26re edition

This is [3D Controller Overlay](https://github.com/ceski-1/3d-controller-overlay) by ceski (MIT licence, see `LICENSE`), adapted for the **sc26re** firmware (Steam Controller 2026, v51 or newer):

- **What your hands do, even with the remapper on.** The overlay reads the controller's physical state from the firmware (`sc26re/raw`, about 250–500 times a second) on a second handle to the same HID interface: buttons, back buttons, triggers, sticks, both trackpads with pressure, stick and grip touch. The remapper can send keys, mouse or nothing to the PC, the model still shows every input.
- **Exact gyro.** The 3D model turns with the controller's own 1 kHz gyro integration (a quaternion), so it is right at any read rate, and it keeps working while the remapper's gyro mouse takes the gyro away from the PC.
- Stock firmware, other controllers: as before (SDL).

The *Controller* section of the settings window says whether the native state is used ("sc26re: physical inputs and gyro, N reads/s"). Pick the **Steam Controller** model. Works over the USB cable, the Puck and Bluetooth.

Windows builds: the [3dco-sc26re](https://github.com/pablo89898991/sc26re-1khz/releases/tag/3dco-sc26re) release (built by this repository's "3dco" workflow).

---

3D Controller Overlay is a simple program for content creaters to show what their controller is doing in 3D space without the need for a handcam.

[![screenshot](res/screenshot.png)](https://github.com/ceski-1/3d-controller-overlay/releases/latest)

## Download

This software is in early development. Windows "artifacts" are available on the [Actions tab](https://github.com/ceski-1/3d-controller-overlay/actions). A GitHub account is required to download them. To build from source, see [Compiling](#compiling).

## Compiling

Requirements:

- [CMake](https://cmake.org) (3.16 or later)
- [glad](https://github.com/Dav1dde/glad/tree/v0.1.36) (0.1.36, gl 3.3, core profile)
- [GLFW](https://github.com/glfw/glfw) (3.4 or later)
- [glm](https://github.com/g-truc/glm) (1.0.3 or later)
- [imgui](https://github.com/ocornut/imgui) (1.92.8 or later)
- [imgui-filebrowser](https://github.com/AirGuanZ/imgui-filebrowser) (latest)
- [SDL3](https://github.com/libsdl-org/SDL) (latest)
- [stb](https://github.com/nothings/stb) (stb_image.h 2.30 or later)

For Windows, [vcpkg](https://github.com/microsoft/vcpkg) is recommended. For Linux, use the libraries provided by your distribution.

Clone the repository:

```
git clone https://github.com/ceski-1/3d-controller-overlay.git
```

Configure (if using vcpkg):

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE="<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake"
```

Configure (if not using vcpkg):

```
cmake -B build
```

Build:

```
cmake --build build --config Release
```

## License

3D Controller Overlay is licensed under the MIT License, see [LICENSE](https://github.com/ceski-1/3d-controller-overlay/blob/main/LICENSE) for more information.
