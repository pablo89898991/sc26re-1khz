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
