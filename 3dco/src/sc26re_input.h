// sc26re (Steam Controller 2026 custom firmware, v51+) native input for 3D Controller Overlay.
//
// With the sc26re remapper on, the pad report the PC sees only has what the remapper sends
// (mapped buttons leave it, the gyro mouse takes the gyro). The firmware also answers a
// feature-report path, "sc26re/raw", with the physical state before any remapping: buttons,
// triggers, sticks, trackpads with pressure, accel, and the gyro integrated at 1 kHz as a
// quaternion. A thread reads it on a second handle to the same HID interface SDL uses; the
// overlay then shows what the hands do, whatever the remapper sends.
#pragma once

#include <SDL3/SDL.h>

struct sc26re_input;

// Starts reading if this gamepad is a Steam Controller 2026 (wired, Bluetooth or a Puck) whose
// firmware answers sc26re/raw; NULL otherwise (stock firmware is found out in the thread and
// the reader then stops by itself: sc26re_active() stays false).
sc26re_input *sc26re_open(SDL_Gamepad *gamepad);
void sc26re_close(sc26re_input *s);

// true while fresh native states arrive (the last one under 0.25 s ago)
bool sc26re_active(sc26re_input *s);

// The newest physical state, mapped like SDL's Steam Controller (Triton) driver maps the pad
// report, so the overlay can use it in place of SDL_GetGamepadButton / Axis / Touchpad / CapSense.
bool sc26re_button(sc26re_input *s, SDL_GamepadButton button);
Sint16 sc26re_axis(sc26re_input *s, SDL_GamepadAxis axis);
bool sc26re_finger(sc26re_input *s, int touchpad, bool *down, float *x, float *y, float *pressure);
bool sc26re_capsense(sc26re_input *s, SDL_GamepadCapSenseType type);

// The turn since the last call, in SDL's gyro frame (x pitch, y yaw, z roll), radians, as an
// axis * angle vector. Exact at any read rate: it comes from the controller's own 1 kHz
// integration. Returns false when there is nothing new.
bool sc26re_take_rotation(sc26re_input *s, float rot[3]);

// native states per second (for the status line)
int sc26re_rate(sc26re_input *s);
