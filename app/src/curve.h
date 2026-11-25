/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdint.h>

/*
 * Custom response curve (Steam's 4-point "Custom" response): the line runs from (0, 0)
 * through the 4 points to (32767, 32767). Points are x, y pairs in 0..32767 and may come in
 * any order; points at the same x make a step. x and the result are fractions of full scale
 * (0..1); above 1 the last segment carries on (the gyro mouse's speed has no top).
 */
static inline float curve_eval(const uint16_t pt[8], float x)
{
	float xs[6], ys[6];
	int n = 1;

	if(x <= 0.0f)
	{
		return 0.0f;
	}
	xs[0] = 0.0f;
	ys[0] = 0.0f;
	for(int i = 0; i < 4; ++i)
	{
		float px = (float)(pt[2 * i] > 32767U ? 32767U : pt[2 * i]) / 32767.0f;
		float py = (float)(pt[2 * i + 1] > 32767U ? 32767U : pt[2 * i + 1]) / 32767.0f;
		int k = n;

		/* insertion by x; equal x keeps the given order */
		while(k > 1 && xs[k - 1] > px)
		{
			xs[k] = xs[k - 1];
			ys[k] = ys[k - 1];
			--k;
		}
		xs[k] = px;
		ys[k] = py;
		++n;
	}
	xs[n] = 1.0f;
	ys[n] = 1.0f;
	++n;
	for(int k = 0; k + 1 < n; ++k)
	{
		float w = xs[k + 1] - xs[k];

		if(w > 0.0f && x <= xs[k + 1])
		{
			return ys[k] + (ys[k + 1] - ys[k]) * (x - xs[k]) / w;
		}
	}
	/* x above the last point: the last segment with a width, carried on */
	for(int k = n - 2; k >= 0; --k)
	{
		float w = xs[k + 1] - xs[k];

		if(w > 0.0f)
		{
			return ys[k + 1] + (ys[k + 1] - ys[k]) / w * (x - xs[k + 1]);
		}
	}
	return x;
}
