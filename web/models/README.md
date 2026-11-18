# Steam Controller 3D models

## steam-controller-valve.glb (default in overlay.html)

© 2026 Valve Corporation, from Valve's Steam Controller CAD files
(https://gitlab.steamos.cloud/SteamHardware/SteamController), licensed under
[CC BY-NC-SA 4.0](http://creativecommons.org/licenses/by-nc-sa/4.0/) — no commercial use. See
`LICENSE-valve.txt` and `ATTRIBUTION-valve.txt`. This is a low-poly version of the model split into
parts (OBJ files), converted unchanged into one GLB with `tools/model/obj2glb.py`; placement and
motion of the parts are done by `overlay.html`. This file is shared under the same license.
Not affiliated with or endorsed by Valve.

## steam-controller.glb ("detailed, textured")

`steam-controller.glb` is taken from the
[steam-controller-toolkit](https://github.com/robinneefs/steam-controller-toolkit) project
(`public/models/`), which adapted it from
**["Steam Machine + Controller"](https://sketchfab.com/3d-models/steam-machine-controller-c1fcf77d71984c11818a5e2412816705)
by [dark_igorek](https://sketchfab.com/dark_igorek)**, licensed under
[Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/).

Changes made by steam-controller-toolkit: only the controller was kept, scaled to centimetres and
centred, its buttons, sticks, trackpads, triggers and back buttons split into separate parts, the
D-pad cut into four arms, textures re-encoded as WebP, geometry quantized and meshopt-compressed.
Used here unchanged by `overlay.html`. See `LICENSE.txt`. Neither author endorses this project.

three.js (`../vendor/three/`, r170) is MIT licensed, see `../vendor/three/LICENSE`.
