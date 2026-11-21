/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "controller.h"
#include "sc26re_cfg.h"
#include "xinput.h"

/*
 * The Steam button as Xbox Guide (option xi_guide): sent as a 100 ms tap when
 * the Steam button is let go without any other button pressed meanwhile, so
 * Steam + D-pad (profiles), Steam + Y (power) and other chords never open the
 * Xbox Game Bar, and a long hold (Steam menu, power off) never does either. Pure function of the buttons and the time (tested on the PC).
 */
bool xinput_guide_tap(uint32_t buttons, int64_t now)
{
	static bool down, combo;
	static int64_t until, t_down;
	bool steam = (buttons & BIT(CONTROLLER_BUTTON_STEAM)) != 0U;

	if(sc26re_cfg_get(SC26RE_CFG_XI_GUIDE) != 1)
	{
		down = false;
		return false;
	}
	if(steam)
	{
		if(!down)
		{
			down = true;
			combo = false;
			t_down = now;
		}
		combo = combo || (buttons & ~(BIT(CONTROLLER_BUTTON_STEAM) | XI_GUIDE_IGNORE)) != 0U;
	}
	else if(down)
	{
		down = false;
		if(!combo && now - t_down < 500) /* a short tap: a long hold is Steam's (menu, power) */
		{
			until = now + 100;
		}
	}
	return now < until;
}

/*
 * The Steam button to Steam: held back while it is held alone, so the controller's own
 * Steam chords never reach Steam (v51 test 63). Steam + D-pad (profiles) and Steam + Y
 * (power off) used to open the Steam overlay / menu on every profile change: Steam saw
 * Steam pressed and let go.
 *
 * - let go within STEAM_DECIDE_MS (0.8 s) with nothing else pressed: a 100 ms tap goes to Steam
 *   (the menu / overlay opens as before, a moment later)
 * - a D-pad direction or Y joins first: the controller's chord, Steam never sees the button
 * - any other button joins, or it is held STEAM_DECIDE_MS: Steam gets it from then on, so
 *   Steam's own chords (Steam + X keyboard, Steam + RT screenshot ...) and the long hold work
 *
 * out = the report's buttons, phys = the physical buttons (the chord is read from these).
 * Pure function of the buttons and the time (tested in the simulator).
 */
#define STEAM_DECIDE_MS 800
#define STEAM_TAP_MS 100

uint32_t steam_button_hold_back(uint32_t out, uint32_t phys, int64_t now)
{
	static bool down;
	static uint8_t mode; /* 0 = not decided yet, 1 = to Steam, 2 = hidden (controller's chord) */
	static int64_t t_down, tap_until;
	const uint32_t steam = BIT(CONTROLLER_BUTTON_STEAM);
	const uint32_t fw_chord = BIT(CONTROLLER_BUTTON_DPAD_UP) | BIT(CONTROLLER_BUTTON_DPAD_DOWN) |
	                          BIT(CONTROLLER_BUTTON_DPAD_LEFT) | BIT(CONTROLLER_BUTTON_DPAD_RIGHT) |
	                          BIT(CONTROLLER_BUTTON_Y) | BIT(CONTROLLER_BUTTON_X) | BIT(CONTROLLER_BUTTON_A) |
	                          BIT(CONTROLLER_BUTTON_MENU) | BIT(CONTROLLER_BUTTON_VIEW); /* remap.c's combinations */

	if(out & steam)
	{
		uint32_t others = phys & ~(steam | XI_GUIDE_IGNORE);

		if(!down)
		{
			down = true;
			mode = 0U;
			t_down = now;
			tap_until = 0;
		}
		if(mode == 0U)
		{
			if(others & fw_chord)
			{
				mode = 2U;
			}
			else if(others != 0U || now - t_down >= STEAM_DECIDE_MS)
			{
				mode = 1U;
			}
		}
		if(mode != 1U)
		{
			out &= ~steam;
		}
		return out;
	}
	if(down)
	{
		down = false;
		if(mode == 0U)
		{
			tap_until = now + STEAM_TAP_MS;
		}
	}
	if(now < tap_until)
	{
		out |= steam;
	}
	return out;
}
