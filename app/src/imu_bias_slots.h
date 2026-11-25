/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * One gyro calibration per sensor full-scale range. The slot number is the LSM6DSV16X
 * FS_G code: 1 = 250, 2 = 500, 3 = 1000, 4 = 2000 dps. Slot 0 is unused.
 * A range that was never calibrated has no entry; the bias in use is kept until it is.
 */
#define IMU_BIAS_SLOT_COUNT 5

struct imu_bias_slots
{
	float bias[IMU_BIAS_SLOT_COUNT][3];
	bool valid[IMU_BIAS_SLOT_COUNT];
};

/* gyro full scale in dps (as the setting holds it) -> FS_G code / slot */
static inline uint8_t imu_bias_slot_for_fsr(int16_t fsr)
{
	return fsr <= 250 ? 1U : fsr <= 500 ? 2U : fsr <= 1000 ? 3U : 4U;
}

static inline void imu_bias_slots_store(struct imu_bias_slots *s, unsigned int slot,
                                        const float bias[3])
{
	if(slot > 0U && slot < IMU_BIAS_SLOT_COUNT)
	{
		memcpy(s->bias[slot], bias, sizeof(s->bias[slot]));
		s->valid[slot] = true;
	}
}

/* the calibration for this range: true and copied to out when the range has one */
static inline bool imu_bias_slots_select(const struct imu_bias_slots *s, unsigned int slot,
                                         float out[3])
{
	if(slot > 0U && slot < IMU_BIAS_SLOT_COUNT && s->valid[slot])
	{
		memcpy(out, s->bias[slot], sizeof(s->bias[slot]));
		return true;
	}
	return false;
}

static inline void imu_bias_slots_clear(struct imu_bias_slots *s)
{
	memset(s, 0, sizeof(*s));
}
