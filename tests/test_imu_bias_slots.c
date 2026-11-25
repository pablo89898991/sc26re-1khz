/* host test for app/src/imu_bias_slots.h: gcc -I app/src -o /tmp/t tests/test_imu_bias_slots.c && /tmp/t */
#include <stdio.h>
#include <stdlib.h>
#include "imu_bias_slots.h"

static int fails;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while(0)

int main(void)
{
	struct imu_bias_slots s;
	float b2000[3] = { -0.0058f, -0.0021f, 0.0197f };
	float b250[3] = { -0.0041f, -0.0009f, 0.0213f };
	float cur[3] = { 0.1f, 0.2f, 0.3f }, out[3];

	imu_bias_slots_clear(&s);
	/* range -> slot (FS_G code) */
	CHECK(imu_bias_slot_for_fsr(125) == 1 && imu_bias_slot_for_fsr(250) == 1);
	CHECK(imu_bias_slot_for_fsr(251) == 2 && imu_bias_slot_for_fsr(500) == 2);
	CHECK(imu_bias_slot_for_fsr(1000) == 3 && imu_bias_slot_for_fsr(2000) == 4);
	CHECK(imu_bias_slot_for_fsr(32767) == 4);

	/* nothing calibrated: nothing selected, bias in use stays */
	memcpy(out, cur, sizeof(out));
	CHECK(!imu_bias_slots_select(&s, 4, out));
	CHECK(memcmp(out, cur, sizeof(out)) == 0);

	/* calibrate at 2000, then at 250: each range keeps its own value */
	imu_bias_slots_store(&s, imu_bias_slot_for_fsr(2000), b2000);
	imu_bias_slots_store(&s, imu_bias_slot_for_fsr(250), b250);
	CHECK(imu_bias_slots_select(&s, 4, out) && memcmp(out, b2000, sizeof(out)) == 0);
	CHECK(imu_bias_slots_select(&s, 1, out) && memcmp(out, b250, sizeof(out)) == 0);
	/* the range in between was never calibrated */
	memcpy(out, cur, sizeof(out));
	CHECK(!imu_bias_slots_select(&s, 2, out) && memcmp(out, cur, sizeof(out)) == 0);

	/* toggling back and forth (remapper on/off) never mixes them up */
	for(int i = 0; i < 100; ++i)
	{
		unsigned int slot = (i & 1) ? 4U : 1U;
		CHECK(imu_bias_slots_select(&s, slot, out));
		CHECK(memcmp(out, (i & 1) ? b2000 : b250, sizeof(out)) == 0);
	}

	/* recalibrating one range leaves the other alone */
	{
		float n[3] = { 0.001f, 0.002f, 0.003f };
		imu_bias_slots_store(&s, 4, n);
		CHECK(imu_bias_slots_select(&s, 1, out) && memcmp(out, b250, sizeof(out)) == 0);
	}

	/* bad slot numbers are ignored */
	imu_bias_slots_store(&s, 0, b250);
	imu_bias_slots_store(&s, 5, b250);
	imu_bias_slots_store(&s, 99, b250);
	CHECK(!imu_bias_slots_select(&s, 0, out));
	CHECK(!imu_bias_slots_select(&s, 5, out));
	CHECK(!imu_bias_slots_select(&s, 99, out));

	/* reset clears all */
	imu_bias_slots_clear(&s);
	for(unsigned int i = 0; i < IMU_BIAS_SLOT_COUNT; ++i) CHECK(!imu_bias_slots_select(&s, i, out));

	printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
