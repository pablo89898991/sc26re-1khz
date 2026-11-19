/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <math.h>
#include <stdint.h>

/*
 * The controller's orientation for the 3D overlay (sc26re/raw): the gyro integrated since
 * power-on as a quaternion (turns about the controller's own axes). A reader keeps the one it
 * saw last and turns by the difference. That is exact however seldom it reads (turns about
 * changing axes do not add up axis by axis), several readers (two overlays, the remapper page)
 * never take motion from each other, and a reply that is lost, comes twice or comes late loses
 * or repeats nothing. (Up to v51 test 42 every read took the turn since the last read.)
 */
struct imu_raw_orient
{
	float q[4]; /* w, x, y, z */
	float r[3]; /* a small turn not in q yet (rad): steps of a few microradians would round away against q */
};

static inline void imu_raw_orient_init(struct imu_raw_orient *o)
{
	o->q[0] = 1.0f;
	o->q[1] = o->q[2] = o->q[3] = 0.0f;
	o->r[0] = o->r[1] = o->r[2] = 0.0f;
}

/* out = q * (the turn r). Called with the IMU data locked (interrupts off): no sinf / cosf for
 * the small turns of every sample, their series is exact in float below 0.1 rad. */
static inline void imu_raw_orient_turn(const float q[4], const float r[3], float out[4])
{
	float n2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
	float c, s; /* cos(|r| / 2), sin(|r| / 2) / |r| */

	if(n2 < 0.01f)
	{
		float h2 = 0.25f * n2;

		c = 1.0f - h2 * (0.5f - h2 * (1.0f / 24.0f));
		s = 0.5f * (1.0f - h2 * ((1.0f / 6.0f) - h2 * (1.0f / 120.0f)));
	}
	else
	{
		float a = sqrtf(n2);

		c = cosf(0.5f * a);
		s = sinf(0.5f * a) / a;
	}
	{
		const float d[4] = { c, r[0] * s, r[1] * s, r[2] * s };
		float o[4] = {
			q[0] * d[0] - q[1] * d[1] - q[2] * d[2] - q[3] * d[3],
			q[0] * d[1] + q[1] * d[0] + q[2] * d[3] - q[3] * d[2],
			q[0] * d[2] - q[1] * d[3] + q[2] * d[0] + q[3] * d[1],
			q[0] * d[3] + q[1] * d[2] - q[2] * d[1] + q[3] * d[0],
		};
		float k = 1.0f / sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);

		for(int i = 0; i < 4; ++i)
		{
			out[i] = o[i] * k;
		}
	}
}

/* one gyro sample in report units (0.06103 deg/s) held for dt seconds */
static inline void imu_raw_orient_add(struct imu_raw_orient *o, const int16_t gyro[3], float dt)
{
	const float k = 0.06103f * 0.017453293f * dt;

	for(int i = 0; i < 3; ++i)
	{
		o->r[i] += (float)gyro[i] * k;
	}
	/* collect up to 1 mrad (the order of turns inside it is below 1e-6 rad), then turn q */
	if(o->r[0] * o->r[0] + o->r[1] * o->r[1] + o->r[2] * o->r[2] >= 1e-6f)
	{
		imu_raw_orient_turn(o->q, o->r, o->q);
		o->r[0] = o->r[1] = o->r[2] = 0.0f;
	}
}

/* the orientation now, w x y z scaled by 32767 */
static inline void imu_raw_orient_get(const struct imu_raw_orient *o, int16_t out[4])
{
	float q[4];

	imu_raw_orient_turn(o->q, o->r, q);
	for(int i = 0; i < 4; ++i)
	{
		out[i] = (int16_t)lrintf(q[i] * 32767.0f);
	}
}
