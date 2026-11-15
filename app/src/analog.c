/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "controller.h"

#include "analog.h"
#include "calibration.h"
#include "haptics.h"
#include "ibex_settings_registry.h"

LOG_MODULE_REGISTER(analog);

#define ANALOG_NODE DT_PATH(ibex_analog_inputs)
#define ANALOG_CHANNEL_COUNT 6
#define ANALOG_SAMPLE_COUNT 4
#define ANALOG_POWER_SETTLE_US 200
#define STICK_CENTER_MV 1650
#define STICK_ENDPOINT_MARGIN_PERCENT 2
#define TRIGGER_FULL_SCALE_MV 1650
#define TRIGGER_ENDPOINT_TRIM_PERCENT 2
#define TRIGGER_RANGE_MARGIN_PERCENT 15
#define TRIGGER_CLICK_THRESHOLD_SETTING SETTING_TRIGGER_THRESHOLD_PERCENT
#define TRIGGER_MODE_SETTING 62
static uint8_t trigger_mode_dbg;
#define TRIGGER_CLICK_RELEASE_HYSTERESIS_PERCENT 5
#define PUCK_PILOT_PRESENT_MIN_MV 2000

#if CONFIG_IBEX_BATTERY
#define BATTERY_METER_NODE DT_NODELABEL(battery_meter)
#define BATTERY_METER_POWER_SETTLE_US DT_PROP(BATTERY_METER_NODE, power_on_sample_delay_us)
#define BATTERY_METER_MAX_SAMPLES 40
#endif

enum analog_channel
{
	ANALOG_STICK_LEFT_X,
	ANALOG_STICK_LEFT_Y,
	ANALOG_STICK_RIGHT_X,
	ANALOG_PUCK_PILOT = ANALOG_STICK_RIGHT_X,
	ANALOG_STICK_RIGHT_Y,
	ANALOG_TRIGGER_LEFT,
	ANALOG_TRIGGER_RIGHT,
};

static const struct adc_dt_spec analog_channels[] = {
	ADC_DT_SPEC_GET_BY_NAME(ANALOG_NODE, stick_left_x),
	ADC_DT_SPEC_GET_BY_NAME(ANALOG_NODE, stick_left_y),
	ADC_DT_SPEC_GET_BY_NAME(ANALOG_NODE, stick_right_x),
	ADC_DT_SPEC_GET_BY_NAME(ANALOG_NODE, stick_right_y),
	ADC_DT_SPEC_GET_BY_NAME(ANALOG_NODE, trigger_left),
	ADC_DT_SPEC_GET_BY_NAME(ANALOG_NODE, trigger_right),
};
static const struct gpio_dt_spec analog_enable = GPIO_DT_SPEC_GET(ANALOG_NODE, enable_gpios);
#if CONFIG_IBEX_BATTERY
static const struct voltage_divider_dt_spec battery_meter =
    VOLTAGE_DIVIDER_DT_SPEC_GET(BATTERY_METER_NODE);
static const struct gpio_dt_spec battery_meter_enable =
    GPIO_DT_SPEC_GET(BATTERY_METER_NODE, power_gpios);
#endif
static bool trigger_left_click_latched;
static bool trigger_right_click_latched;

static int16_t trigger_to_report(int32_t sample, const struct trigger_calibration *cal);

/*
 * Direction of a trigger, decided at power-on from where it rests.
 * Valve's factory records are not consistent about the "inverted" byte (seen: left 1, right 0
 * on a controller whose two triggers both read ~1400 mV released and ~570 mV pressed), so a
 * factory record could make a released trigger read 100 %. At boot the triggers are released:
 * the endpoint the trigger sits at during the first 300 ms is taken as "released".
 * A calibration the user makes later (validated at save time) is used as is.
 */
#define TRIGGER_DIR_WARMUP_SAMPLES 500 /* ignore ADC/filter start-up */
#define TRIGGER_DIR_BOOT_SAMPLES (TRIGGER_DIR_WARMUP_SAMPLES + 300)
static int8_t trigger_dir_inverted[2] = { -1, -1 };
static uint16_t trigger_dir_hits[2][2];
static struct trigger_calibration trigger_dir_for[2];
static uint32_t trigger_dir_samples[2];
static bool trigger_dir_decided[2];

static void trigger_apply_direction(int i, int32_t raw, struct trigger_calibration *cal)
{
	if(!calibration_trigger_valid(cal))
	{
		return;
	}
	/*
	 * The physical direction never changes, so once decided it stays for this power cycle,
	 * whatever "inverted" byte a later calibration write carries (Steam's controller settings
	 * re-send calibration records whose flag does not match the hardware).
	 */
	trigger_dir_for[i] = *cal;
	if(trigger_dir_samples[i] < TRIGGER_DIR_BOOT_SAMPLES)
	{
		int32_t lo = MIN(cal->pressed, cal->idle), hi = MAX(cal->pressed, cal->idle);
		int32_t band = (hi - lo) / 8;

		trigger_dir_samples[i]++;
		if(trigger_dir_samples[i] > TRIGGER_DIR_WARMUP_SAMPLES)
		{
			if(raw >= hi - band)
			{
				trigger_dir_hits[i][1]++;
			}
			else if(raw <= lo + band)
			{
				trigger_dir_hits[i][0]++;
			}
		}
		if(trigger_dir_samples[i] == TRIGGER_DIR_BOOT_SAMPLES)
		{
			const uint32_t window = TRIGGER_DIR_BOOT_SAMPLES - TRIGGER_DIR_WARMUP_SAMPLES;

			if(trigger_dir_hits[i][1] > window * 9 / 10)
			{
				trigger_dir_inverted[i] = 1; /* rests high: pressing lowers the voltage */
				trigger_dir_decided[i] = true;
			}
			else if(trigger_dir_hits[i][0] > window * 9 / 10)
			{
				trigger_dir_inverted[i] = 0;
				trigger_dir_decided[i] = true;
			}
			if(trigger_dir_inverted[i] >= 0 && trigger_dir_inverted[i] != (cal->inverted != 0))
			{
				LOG_WRN("%s trigger: calibration direction disagrees with rest position, "
				        "using inverted=%d", i ? "right" : "left", trigger_dir_inverted[i]);
			}
		}
	}
	if(trigger_dir_inverted[i] >= 0)
	{
		cal->inverted = (uint8_t)trigger_dir_inverted[i];
	}
	else
	{
		/* not decided yet (start-up, or trigger held at power-on): the SC2 triggers rest at
		 * the high-voltage end (~1.4 V released, ~0.57 V pressed on both sides) */
		cal->inverted = 1;
	}
}

#if CONFIG_IBEX_TRIGGER_DEBUG_IN_QUAT
static int32_t trg_dbg_raw[2];
static struct trigger_calibration trg_dbg_cal[2];

/* alternates left/right: w = raw mV, x = cal pressed, y = cal idle, z = side<<8 | inverted<<4 | type */
void analog_trigger_debug_quat(struct controller_report *report)
{
	static uint8_t side;

	side ^= 1U;
	report->gyro_quat_w = (int16_t)CLAMP(trg_dbg_raw[side], INT16_MIN, INT16_MAX);
	report->gyro_quat_x = (int16_t)trg_dbg_cal[side].pressed;
	report->gyro_quat_y = (int16_t)trg_dbg_cal[side].idle;
	report->gyro_quat_z = (int16_t)(((trigger_mode_dbg & 3) << 12) | (side << 8) | ((trg_dbg_cal[side].inverted & 0xf) << 4) |
	                                (trigger_dir_decided[side] ? 8 : 0) | (trg_dbg_cal[side].type & 7));
}
#endif

/* Trigger step calibration (port of OFW 6AE90BC5 get_id_set_trigger_step_cal). */
#define TRIGGER_CAL_MIN_SPAN_MV 100

/*
 * Rest auto-zero. Each unit's released trigger sits at a slightly different
 * voltage, and on some units it is far enough from the factory "idle" point
 * that a released trigger reads as partly pressed (in our own scaling and in
 * Steam's, which uses the same records in trigger_mode 1/2). At power-on the
 * resting voltage is learned; afterwards the travel from that rest to the
 * pressed endpoint is stretched onto idle..pressed, so a released trigger
 * always lands exactly on the calibration's idle point (= 0 %).
 */
#define TRIGGER_REST_STABLE_MV 60
static int32_t trigger_rest_sum[2];
static int32_t trigger_rest_min[2] = { INT32_MAX, INT32_MAX };
static int32_t trigger_rest_max[2] = { INT32_MIN, INT32_MIN };
static uint32_t trigger_rest_samples[2];
static int32_t trigger_rest_mv[2];
static bool trigger_rest_valid[2];

static void trigger_learn_rest(int i, int32_t raw)
{
	if(trigger_rest_samples[i] >= TRIGGER_DIR_BOOT_SAMPLES)
	{
		return;
	}
	trigger_rest_samples[i]++;
	if(trigger_rest_samples[i] <= TRIGGER_DIR_WARMUP_SAMPLES)
	{
		return;
	}
	trigger_rest_sum[i] += raw;
	trigger_rest_min[i] = MIN(trigger_rest_min[i], raw);
	trigger_rest_max[i] = MAX(trigger_rest_max[i], raw);
	if(trigger_rest_samples[i] == TRIGGER_DIR_BOOT_SAMPLES)
	{
		int32_t n = TRIGGER_DIR_BOOT_SAMPLES - TRIGGER_DIR_WARMUP_SAMPLES;

		trigger_rest_mv[i] = trigger_rest_sum[i] / n;
		/* A trigger that moved during the window was being touched: do not learn. */
		trigger_rest_valid[i] = trigger_rest_max[i] - trigger_rest_min[i] <= TRIGGER_REST_STABLE_MV;
		LOG_INF("%s trigger rest %d mV (%d..%d) %s", i ? "right" : "left", trigger_rest_mv[i],
		        trigger_rest_min[i], trigger_rest_max[i],
		        trigger_rest_valid[i] ? "learned" : "unstable, not used");
	}
}

/*
 * Released-trigger dead zone, in permille of the idle..pressed travel. ADC
 * noise at 1 kHz made a released trigger wander up to ~6 % (~2000) on some units,
 * which Steam's desktop layout (RT = left click) can see as a press. The
 * travel beyond the dead zone is stretched so full press still reaches 100 %.
 */
#define TRIGGER_REST_DEADZONE_PERMILLE 80
/* Light low-pass on the trigger voltage: y += (x - y) / 4 per 1 ms sample (~4 ms). */
#define TRIGGER_FILTER_SHIFT 2
static int32_t trigger_filt_q8[2];
static bool trigger_filt_init[2];

static int32_t trigger_filter(int i, int32_t raw)
{
	if(!trigger_filt_init[i])
	{
		trigger_filt_q8[i] = raw << 8;
		trigger_filt_init[i] = true;
	}
	trigger_filt_q8[i] += ((raw << 8) - trigger_filt_q8[i]) >> TRIGGER_FILTER_SHIFT;
	return trigger_filt_q8[i] >> 8;
}

/* Physical released / pressed ends of a calibration record (in mV). */
static bool trigger_endpoints(int i, const struct trigger_calibration *cal, int32_t *e_idle,
                              int32_t *e_press)
{
	int32_t lo = MIN(cal->idle, cal->pressed);
	int32_t hi = MAX(cal->idle, cal->pressed);

	if(!calibration_trigger_valid(cal))
	{
		return false;
	}
	if(trigger_rest_valid[i])
	{
		bool idle_near = abs(trigger_rest_mv[i] - (int32_t)cal->idle) <=
		                 abs(trigger_rest_mv[i] - (int32_t)cal->pressed);

		*e_idle = idle_near ? cal->idle : cal->pressed;
		*e_press = idle_near ? cal->pressed : cal->idle;
	}
	else
	{
		*e_idle = cal->inverted ? hi : lo;
		*e_press = cal->inverted ? lo : hi;
	}
	return true;
}

/*
 * Map a measured trigger voltage so the learned rest equals the calibration
 * idle point, then apply the released-trigger dead zone.
 */
static int32_t trigger_rest_remap(int i, int32_t raw, const struct trigger_calibration *cal)
{
	int32_t rest = trigger_rest_mv[i];
	int32_t e_idle;
	int32_t e_press;
	int32_t out = raw;
	int32_t span;
	int32_t permille;

	if(!calibration_trigger_valid(cal))
	{
		return raw;
	}
	if(trigger_rest_valid[i])
	{
		/* The physical released end is the endpoint nearer to where the trigger rests. */
		if(abs(rest - (int32_t)cal->idle) <= abs(rest - (int32_t)cal->pressed))
		{
			e_idle = cal->idle;
			e_press = cal->pressed;
		}
		else
		{
			e_idle = cal->pressed;
			e_press = cal->idle;
		}
	}
	else
	{
		/* Direction as decided by trigger_apply_direction (inverted = rests high). */
		int32_t lo = MIN(cal->idle, cal->pressed);
		int32_t hi = MAX(cal->idle, cal->pressed);

		e_idle = cal->inverted ? hi : lo;
		e_press = cal->inverted ? lo : hi;
	}
	span = e_press - e_idle;

	if(trigger_rest_valid[i])
	{
		int32_t travel = e_press - rest;

		/* Rest must lie on the released side of the pressed end with real travel left. */
		if(abs(travel) >= TRIGGER_CAL_MIN_SPAN_MV / 2 && (travel > 0) == (span > 0))
		{
			out = e_idle + (int32_t)(((int64_t)(raw - rest) * span) / travel);
		}
	}

	permille = (int32_t)(((int64_t)(out - e_idle) * 1000) / span);
	if(permille <= TRIGGER_REST_DEADZONE_PERMILLE)
	{
		return e_idle;
	}
	permille = MIN(permille, 1000);
	permille = ((permille - TRIGGER_REST_DEADZONE_PERMILLE) * 1000) /
	           (1000 - TRIGGER_REST_DEADZONE_PERMILLE);
	return e_idle + (int32_t)(((int64_t)permille * span) / 1000);
}
static struct k_spinlock trigger_cal_lock;
static uint8_t trigger_cal_stage;
static bool trigger_cal_seen[2][2]; /* [trigger][stage 1/2] */
/* [trigger][0] = normal record, [trigger][1] = inverted record, as in OFW */
static struct trigger_calibration trigger_cal_candidate[2][2];
/* sum of stage-1 (released) samples, used to reject a calibration that would read pressed at rest */
static int64_t trigger_cal_rest_sum[2];
static uint32_t trigger_cal_rest_n[2];

/*
 * Stage 4 = "zero triggers": with both triggers released, average their
 * voltage for TRIGGER_ZERO_SAMPLES samples and store it as the released end of
 * the existing calibration (the record field nearer to it), keeping the
 * pressed end. Saved like any calibration, so Steam reads it too.
 */
#define TRIGGER_ZERO_SAMPLES 300
static int64_t trigger_zero_sum[2];
static uint32_t trigger_zero_n;
static void trigger_zero_work_handler(struct k_work *work);
static K_WORK_DEFINE(trigger_zero_work, trigger_zero_work_handler);

static void trigger_zero_work_handler(struct k_work *work)
{
	struct analog_calibration_snapshot snap;

	ARG_UNUSED(work);
	calibration_analog_snapshot(&snap);
	for(int i = 0; i < 2; ++i)
	{
		enum calibration_side side = i ? CALIBRATION_RIGHT : CALIBRATION_LEFT;
		struct trigger_calibration cal = i ? snap.trigger_right : snap.trigger_left;
		int32_t rest = (int32_t)(trigger_zero_sum[i] / TRIGGER_ZERO_SAMPLES);
		int err;

		if(!calibration_trigger_valid(&cal))
		{
			LOG_WRN("zero triggers: %s trigger has no valid calibration, skipped",
			        i ? "right" : "left");
			continue;
		}
		if(abs(rest - (int32_t)cal.idle) <= abs(rest - (int32_t)cal.pressed))
		{
			cal.idle = (uint16_t)CLAMP(rest, 0, INT16_MAX);
		}
		else
		{
			cal.pressed = (uint16_t)CLAMP(rest, 0, INT16_MAX);
		}
		if(!calibration_trigger_valid(&cal))
		{
			LOG_WRN("zero triggers: %s trigger rest %d mV leaves no travel, skipped",
			        i ? "right" : "left", rest);
			continue;
		}
		err = calibration_stage_trigger(side, (const uint8_t *)&cal, sizeof(cal));
		if(!err)
		{
			err = calibration_commit_trigger(side);
		}
		/* the boot-learned rest now equals the stored released end */
		trigger_rest_mv[i] = rest;
		trigger_rest_valid[i] = true;
		LOG_INF("zero triggers %s: rest=%d mV -> pressed=%u idle=%u inverted=%u err=%d",
		        i ? "right" : "left", rest, cal.pressed, cal.idle, cal.inverted, err);
	}
}

static void trigger_cal_sample(const int32_t value[2])
{
	k_spinlock_key_t key = k_spin_lock(&trigger_cal_lock);
	uint8_t stage = trigger_cal_stage;

	if(stage == 4)
	{
		trigger_zero_sum[0] += value[0];
		trigger_zero_sum[1] += value[1];
		if(++trigger_zero_n >= TRIGGER_ZERO_SAMPLES)
		{
			trigger_cal_stage = 0;
			k_work_submit(&trigger_zero_work);
		}
		k_spin_unlock(&trigger_cal_lock, key);
		return;
	}

	for(int i = 0; i < 2 && (stage == 1 || stage == 2); ++i)
	{
		struct trigger_calibration *a = &trigger_cal_candidate[i][0];
		struct trigger_calibration *b = &trigger_cal_candidate[i][1];
		uint16_t v = (uint16_t)CLAMP(value[i], 0, INT16_MAX);

		if(stage == 1)
		{
			trigger_cal_rest_sum[i] += v;
			trigger_cal_rest_n[i]++;
			a->idle = MIN(a->idle, v);
			b->pressed = MAX(b->pressed, v);
		}
		else
		{
			a->pressed = MAX(a->pressed, v);
			b->idle = MIN(b->idle, v);
		}
		trigger_cal_seen[i][stage - 1] = true;
	}
	k_spin_unlock(&trigger_cal_lock, key);
}

int analog_trigger_step_cal(uint8_t stage)
{
	struct trigger_calibration chosen[2];
	bool ok[2];
	k_spinlock_key_t key;

	if(stage > 4)
	{
		return 0;
	}

	key = k_spin_lock(&trigger_cal_lock);
	if(stage == 4)
	{
		trigger_zero_sum[0] = 0;
		trigger_zero_sum[1] = 0;
		trigger_zero_n = 0;
		trigger_cal_stage = 4;
		k_spin_unlock(&trigger_cal_lock, key);
		LOG_INF("zero triggers: sampling released position");
		return 0;
	}
	if(stage == 1)
	{
		for(int i = 0; i < 2; ++i)
		{
			trigger_cal_candidate[i][0] = (struct trigger_calibration){
			    .type = 2, .pressed = 0, .idle = 0x7fff, .inverted = 0};
			trigger_cal_candidate[i][1] = (struct trigger_calibration){
			    .type = 2, .pressed = 0, .idle = 0x7fff, .inverted = 1};
			trigger_cal_seen[i][0] = false;
			trigger_cal_seen[i][1] = false;
			trigger_cal_rest_sum[i] = 0;
			trigger_cal_rest_n[i] = 0;
		}
	}
	if(stage != 3)
	{
		trigger_cal_stage = stage;
		k_spin_unlock(&trigger_cal_lock, key);
		LOG_INF("trigger step calibration stage %u", stage);
		return 0;
	}

	for(int i = 0; i < 2; ++i)
	{
		const struct trigger_calibration *a = &trigger_cal_candidate[i][0];
		const struct trigger_calibration *b = &trigger_cal_candidate[i][1];
		int span_a = abs((int)a->pressed - (int)a->idle);
		int span_b = abs((int)b->pressed - (int)b->idle);

		chosen[i] = span_a < span_b ? *b : *a;
		ok[i] = trigger_cal_seen[i][0] && trigger_cal_seen[i][1] &&
		        MAX(span_a, span_b) >= TRIGGER_CAL_MIN_SPAN_MV;
		if(ok[i] && trigger_cal_rest_n[i] > 0)
		{
			/* sanity: the released position must read (almost) zero with the new values */
			int32_t rest = (int32_t)(trigger_cal_rest_sum[i] / trigger_cal_rest_n[i]);

			ok[i] = trigger_to_report(rest, &chosen[i]) < INT16_MAX / 10;
		}
	}
	trigger_cal_stage = 0;
	k_spin_unlock(&trigger_cal_lock, key);

	for(int i = 0; i < 2; ++i)
	{
		enum calibration_side side = i ? CALIBRATION_RIGHT : CALIBRATION_LEFT;
		int err;

		if(!ok[i])
		{
			LOG_WRN("trigger step calibration: %s trigger range too small, keeping old",
			        i ? "right" : "left");
			continue;
		}
		err = calibration_stage_trigger(side, (const uint8_t *)&chosen[i], sizeof(chosen[i]));
		if(!err)
		{
			err = calibration_commit_trigger(side);
		}
		LOG_INF("trigger step calibration %s: pressed=%u idle=%u inverted=%u err=%d",
		        i ? "right" : "left", chosen[i].pressed, chosen[i].idle, chosen[i].inverted, err);
	}
	return 0;
}
static bool analog_ready;
K_MUTEX_DEFINE(analog_lock);

BUILD_ASSERT(ARRAY_SIZE(analog_channels) == ANALOG_CHANNEL_COUNT);

static int16_t scale_stick_axis(int32_t sample, uint16_t min, uint16_t max, uint16_t center_min,
                                uint16_t center_max)
{
	if(min == 0 && max == 0)
	{
		return CLAMP((sample - STICK_CENTER_MV) * 20, INT16_MIN, INT16_MAX);
	}

	if(sample < center_min)
	{
		int32_t denom = center_min - min;

		if(denom <= 0)
		{
			return 0;
		}
		denom = MAX((denom * (100 - STICK_ENDPOINT_MARGIN_PERCENT)) / 100, 1);
		int64_t val = ((int64_t)(sample - center_min) * 32768) / denom;

		return CLAMP(val, INT16_MIN, 0);
	}
	else if(sample > center_max)
	{
		int32_t denom = max - center_max;

		if(denom <= 0)
		{
			return 0;
		}
		denom = MAX((denom * (100 - STICK_ENDPOINT_MARGIN_PERCENT)) / 100, 1);
		int64_t val = ((int64_t)(sample - center_max) * 32767) / denom;

		return CLAMP(val, 0, INT16_MAX);
	}
	else
	{
		return 0;
	}
}

static int16_t trigger_to_report(int32_t sample, const struct trigger_calibration *cal)
{
	if(!calibration_trigger_valid(cal))
	{
		return CLAMP((sample * INT16_MAX) / TRIGGER_FULL_SCALE_MV, 0, INT16_MAX);
	}

	int32_t low = MIN(cal->pressed, cal->idle);
	int32_t high = MAX(cal->pressed, cal->idle);

	if(cal->inverted)
	{
		low = (low * 100 + (100 - TRIGGER_ENDPOINT_TRIM_PERCENT) - 1) /
		      (100 - TRIGGER_ENDPOINT_TRIM_PERCENT);
	}
	else
	{
		high = (high * (100 - TRIGGER_ENDPOINT_TRIM_PERCENT)) / 100;
	}

	int32_t full_range = high - low;
	if(full_range <= 0)
	{
		return 0;
	}

	int32_t margin = (full_range * TRIGGER_RANGE_MARGIN_PERCENT) / 100;
	int32_t active_low = low + margin;
	int32_t active_high = high - margin;
	int32_t active_range = active_high - active_low;

	if(active_range <= 0)
	{
		return 0;
	}

	int64_t normalized = ((int64_t)(sample - active_low) * INT16_MAX) / active_range;

	if(cal->inverted)
	{
		normalized = INT16_MAX - normalized;
	}

	return CLAMP(normalized, 0, INT16_MAX);
}

static bool trigger_click_active(int16_t value, bool *latched)
{
	int16_t threshold_percent = 90;

	(void)ibex_setting_get(TRIGGER_CLICK_THRESHOLD_SETTING, &threshold_percent);
	threshold_percent = CLAMP(threshold_percent, 0, 100);

	int16_t release_percent = MAX(threshold_percent - TRIGGER_CLICK_RELEASE_HYSTERESIS_PERCENT, 0);
	int32_t press_threshold = ((int32_t)INT16_MAX * threshold_percent) / 100;
	int32_t release_threshold = ((int32_t)INT16_MAX * release_percent) / 100;

	if(*latched)
	{
		*latched = value >= release_threshold;
	}
	else
	{
		*latched = value >= press_threshold;
	}

	return *latched;
}

int analog_init(void)
{
	int err;

	if(!adc_is_ready_dt(&analog_channels[0]))
	{
		LOG_ERR("Ibex SAADC is not ready");
		return -ENODEV;
	}
	if(!gpio_is_ready_dt(&analog_enable))
	{
		LOG_ERR("Ibex analog enable GPIO is not ready");
		return -ENODEV;
	}
#if CONFIG_IBEX_BATTERY
	if(!adc_is_ready_dt(&battery_meter.port))
	{
		LOG_ERR("Ibex battery-meter SAADC is not ready");
		return -ENODEV;
	}
	if(!gpio_is_ready_dt(&battery_meter_enable))
	{
		LOG_ERR("Ibex battery-meter power GPIO is not ready");
		return -ENODEV;
	}
#endif

	err = gpio_pin_configure_dt(&analog_enable, GPIO_OUTPUT_INACTIVE);
	if(err)
	{
		LOG_ERR("analog enable GPIO setup failed: %d", err);
		return err;
	}

	for(size_t i = 0; i < ARRAY_SIZE(analog_channels); ++i)
	{
		err = adc_channel_setup_dt(&analog_channels[i]);
		if(err)
		{
			LOG_ERR("SAADC channel %u setup failed: %d", i, err);
			return err;
		}
	}

#if CONFIG_IBEX_BATTERY
	err = adc_channel_setup_dt(&battery_meter.port);
	if(err)
	{
		LOG_ERR("battery-meter SAADC channel setup failed: %d", err);
		return err;
	}
	err = gpio_pin_configure_dt(&battery_meter_enable, GPIO_OUTPUT_INACTIVE);
	if(err)
	{
		LOG_ERR("battery-meter power GPIO setup failed: %d", err);
		return err;
	}
#endif

	if(IS_ENABLED(CONFIG_SETTINGS))
	{
		err = calibration_load_settings();
		if(err)
		{
			LOG_WRN("failed to load analog calibration settings: %d", err);
		}
	}

	if(IS_ENABLED(CONFIG_SETTINGS) && (!calibration_trigger_loaded(CALIBRATION_LEFT) ||
	                                   !calibration_trigger_loaded(CALIBRATION_RIGHT) ||
	                                   !calibration_stick_loaded(CALIBRATION_LEFT) ||
	                                   !calibration_stick_loaded(CALIBRATION_RIGHT) ||
	                                   !calibration_pressure_loaded(CALIBRATION_LEFT) ||
	                                   !calibration_pressure_loaded(CALIBRATION_RIGHT) ||
	                                   !calibration_battery_voltage_offset_loaded()))
	{
		(void)calibration_import_valve_storage();
	}

	if(!calibration_trigger_loaded(CALIBRATION_LEFT) ||
	   !calibration_trigger_loaded(CALIBRATION_RIGHT) ||
	   !calibration_stick_loaded(CALIBRATION_LEFT) ||
	   !calibration_stick_loaded(CALIBRATION_RIGHT))
	{
		LOG_WRN("Using raw analog fallback for missing calibration: trg_l=%u trg_r=%u joy_l=%u "
		        "joy_r=%u",
		        calibration_trigger_loaded(CALIBRATION_LEFT),
		        calibration_trigger_loaded(CALIBRATION_RIGHT),
		        calibration_stick_loaded(CALIBRATION_LEFT),
		        calibration_stick_loaded(CALIBRATION_RIGHT));
	}
	analog_ready = true;
	return 0;
}

/*
 * Over USB the stick/trigger supply stays on between scans: power-cycling it costs a
 * 200 us settle (plus tick rounding) on every report, which capped the 1 kHz loop.
 * On battery the original per-scan gating is kept.
 */
static bool analog_powered;

/* Fast scan path: wired USB, or the Puck link (which now polls faster than 250 Hz). */
static bool analog_fast_link(void)
{
	return transport_usb_configured() ||
	       (IS_ENABLED(CONFIG_IBEX_ANALOG_FAST_ON_ESB) && transport_esb_connected());
}

static int analog_inputs_take(void)
{
	int err;

	k_mutex_lock(&analog_lock, K_FOREVER);
	if(analog_powered)
	{
		return 0;
	}
	err = gpio_pin_set_dt(&analog_enable, 1);
	if(err)
	{
		k_mutex_unlock(&analog_lock);
		return err;
	}
	k_usleep(ANALOG_POWER_SETTLE_US);
	return 0;
}

static int analog_inputs_put(void)
{
	int err;

	__ASSERT_NO_MSG(analog_lock.owner == k_current_get());

	if(IS_ENABLED(CONFIG_IBEX_ANALOG_STAY_POWERED_ON_USB) && analog_fast_link())
	{
		analog_powered = true;
		k_mutex_unlock(&analog_lock);
		return 0;
	}
	analog_powered = false;
	err = gpio_pin_set_dt(&analog_enable, 0);
	k_mutex_unlock(&analog_lock);
	return err;
}

int analog_puck_pilot_present(bool *present)
{
	const struct adc_dt_spec *channel = &analog_channels[ANALOG_PUCK_PILOT];
	struct adc_sequence sequence = {
		.channels = BIT(channel->channel_id),
		.buffer_size = sizeof(int16_t),
		.resolution = 12,
	};
	int16_t raw;
	int32_t mv;
	int disable_err;
	int err;

	if(present == NULL)
	{
		return -EINVAL;
	}
	*present = false;

	if(!analog_ready)
	{
		return -ENODEV;
	}

	err = analog_inputs_take();
	if(err)
	{
		return err;
	}

	sequence.buffer = &raw;
	err = adc_read(channel->dev, &sequence);
	if(err)
	{
		goto out_disable;
	}

	mv = raw;
	err = adc_raw_to_millivolts_dt(channel, &mv);
	if(err)
	{
		goto out_disable;
	}

	*present = mv >= PUCK_PILOT_PRESENT_MIN_MV;
	err = 0;

out_disable:
	disable_err = analog_inputs_put();
	return err ? err : disable_err;
}

int analog_battery_voltage_samples_mv(uint16_t *samples, size_t sample_count)
{
#if CONFIG_IBEX_BATTERY
	int16_t raw[BATTERY_METER_MAX_SAMPLES];
	struct adc_sequence_options options;
	struct adc_sequence sequence;
	int disable_err;
	int err;

	if(samples == NULL || sample_count == 0U)
	{
		return -EINVAL;
	}
	if(sample_count > ARRAY_SIZE(raw))
	{
		return -E2BIG;
	}
	if(!analog_ready)
	{
		return -ENODEV;
	}

	options = (struct adc_sequence_options){
		.extra_samplings = sample_count - 1U,
	};
	sequence = (struct adc_sequence){
		.options = &options,
		.channels = BIT(battery_meter.port.channel_id),
		.buffer = raw,
		.buffer_size = sample_count * sizeof(raw[0]),
		.resolution = battery_meter.port.resolution,
	};

	err = gpio_pin_set_dt(&battery_meter_enable, 1);
	if(err)
	{
		return err;
	}
	/* The divider can settle while the regular input path continues using the SAADC. */
	k_usleep(BATTERY_METER_POWER_SETTLE_US);

	k_mutex_lock(&analog_lock, K_FOREVER);
	err = adc_read(battery_meter.port.dev, &sequence);
	k_mutex_unlock(&analog_lock);

	disable_err = gpio_pin_set_dt(&battery_meter_enable, 0);
	if(err || disable_err)
	{
		return err ? err : disable_err;
	}

	for(size_t i = 0; i < sample_count; ++i)
	{
		int32_t adc_uv;
		int64_t battery_uv;

		adc_uv = raw[i];
		err = adc_raw_to_microvolts_dt(&battery_meter.port, &adc_uv);
		if(err)
		{
			return err;
		}
		battery_uv = adc_uv;
		err = voltage_divider_scale64_dt(&battery_meter, &battery_uv);
		if(err)
		{
			return err;
		}
		samples[i] = CLAMP(battery_uv / 1000, 0, UINT16_MAX);
	}

	return 0;
#else
	ARG_UNUSED(samples);
	ARG_UNUSED(sample_count);

	return -ENODEV;
#endif
}

int analog_read_report(struct controller_report *report)
{
	int16_t samples[ANALOG_CHANNEL_COUNT * ANALOG_SAMPLE_COUNT];
	int32_t averaged[ANALOG_CHANNEL_COUNT] = { 0 };
	/*
	 * Each extra sampling round costs ~170 us of SAADC time in the report loop; over USB
	 * at 1 kHz fewer rounds are used (CONFIG_IBEX_ANALOG_USB_SAMPLES).
	 */
	const size_t sample_count = analog_fast_link() ?
	    MIN(CONFIG_IBEX_ANALOG_USB_SAMPLES, ANALOG_SAMPLE_COUNT) : ANALOG_SAMPLE_COUNT;
	struct adc_sequence_options options = {
		.extra_samplings = sample_count - 1,
	};
	struct adc_sequence sequence = {
		.options = &options,
		.channels = BIT_MASK(ANALOG_CHANNEL_COUNT),
		.buffer = samples,
		.buffer_size = sizeof(int16_t) * ANALOG_CHANNEL_COUNT * sample_count,
		.resolution = 12,
	};
	int disable_err;
	int err;

	if(!analog_ready)
	{
		return -ENODEV;
	}

	err = analog_inputs_take();
	if(err)
	{
		return err;
	}

	err = adc_read(analog_channels[0].dev, &sequence);
	disable_err = analog_inputs_put();
	if(err || disable_err)
	{
		return err ? err : disable_err;
	}

	for(size_t sample = 0; sample < sample_count; ++sample)
	{
		for(size_t channel = 0; channel < ANALOG_CHANNEL_COUNT; ++channel)
		{
			averaged[channel] += samples[sample * ANALOG_CHANNEL_COUNT + channel];
		}
	}
	int32_t trigger_raw[2];

	for(size_t channel = 0; channel < ANALOG_CHANNEL_COUNT; ++channel)
	{
		averaged[channel] /= (int32_t)sample_count;
		if(channel == ANALOG_TRIGGER_LEFT)
		{
			trigger_raw[0] = averaged[channel];
		}
		else if(channel == ANALOG_TRIGGER_RIGHT)
		{
			trigger_raw[1] = averaged[channel];
		}
		err = adc_raw_to_millivolts_dt(&analog_channels[channel], &averaged[channel]);
		if(err)
		{
			LOG_WRN("SAADC channel %u raw-to-mV conversion failed: %d", channel, err);
			return err;
		}
	}

	struct analog_calibration_snapshot calibration;

	if(trigger_cal_stage == 1 || trigger_cal_stage == 2 || trigger_cal_stage == 4)
	{
		trigger_cal_sample((const int32_t[2]){averaged[ANALOG_TRIGGER_LEFT],
		                                      averaged[ANALOG_TRIGGER_RIGHT]});
	}
	calibration_analog_snapshot(&calibration);
	trigger_learn_rest(0, averaged[ANALOG_TRIGGER_LEFT]);
	trigger_learn_rest(1, averaged[ANALOG_TRIGGER_RIGHT]);
	trigger_apply_direction(0, averaged[ANALOG_TRIGGER_LEFT], &calibration.trigger_left);
	trigger_apply_direction(1, averaged[ANALOG_TRIGGER_RIGHT], &calibration.trigger_right);
	int32_t host_cnt[2] = { trigger_raw[0], trigger_raw[1] };
	int32_t host_mv[2] = { averaged[ANALOG_TRIGGER_LEFT], averaged[ANALOG_TRIGGER_RIGHT] };
	{
		/* Rest auto-zero, applied to millivolts and proportionally to raw counts. */
		int32_t mv_l = trigger_rest_remap(0, trigger_filter(0, averaged[ANALOG_TRIGGER_LEFT]),
		                                  &calibration.trigger_left);
		int32_t mv_r = trigger_rest_remap(1, trigger_filter(1, averaged[ANALOG_TRIGGER_RIGHT]),
		                                  &calibration.trigger_right);

		if(averaged[ANALOG_TRIGGER_LEFT] > 0)
		{
			trigger_raw[0] = (int32_t)(((int64_t)trigger_raw[0] * mv_l) /
			                           averaged[ANALOG_TRIGGER_LEFT]);
		}
		if(averaged[ANALOG_TRIGGER_RIGHT] > 0)
		{
			trigger_raw[1] = (int32_t)(((int64_t)trigger_raw[1] * mv_r) /
			                           averaged[ANALOG_TRIGGER_RIGHT]);
		}
		averaged[ANALOG_TRIGGER_LEFT] = mv_l;
		averaged[ANALOG_TRIGGER_RIGHT] = mv_r;
	}
#if CONFIG_IBEX_TRIGGER_DEBUG_IN_QUAT
	trg_dbg_raw[0] = averaged[ANALOG_TRIGGER_LEFT];
	trg_dbg_raw[1] = averaged[ANALOG_TRIGGER_RIGHT];
	trg_dbg_cal[0] = calibration.trigger_left;
	trg_dbg_cal[1] = calibration.trigger_right;
#endif

	report->stick_left_x = scale_stick_axis(
	    averaged[ANALOG_STICK_LEFT_X], calibration.stick_left.x_min, calibration.stick_left.x_max,
	    calibration.stick_left.x_center_min, calibration.stick_left.x_center_max);
	report->stick_left_y = scale_stick_axis(
	    averaged[ANALOG_STICK_LEFT_Y], calibration.stick_left.y_min, calibration.stick_left.y_max,
	    calibration.stick_left.y_center_min, calibration.stick_left.y_center_max);
	report->stick_right_x =
	    scale_stick_axis(averaged[ANALOG_STICK_RIGHT_X], calibration.stick_right.x_min,
	                     calibration.stick_right.x_max, calibration.stick_right.x_center_min,
	                     calibration.stick_right.x_center_max);
	report->stick_right_y =
	    scale_stick_axis(averaged[ANALOG_STICK_RIGHT_Y], calibration.stick_right.y_min,
	                     calibration.stick_right.y_max, calibration.stick_right.y_center_min,
	                     calibration.stick_right.y_center_max);
	report->trigger_left =
	    trigger_to_report(averaged[ANALOG_TRIGGER_LEFT], &calibration.trigger_left);
	report->trigger_right =
	    trigger_to_report(averaged[ANALOG_TRIGGER_RIGHT], &calibration.trigger_right);
	haptics_trigger_update(report->trigger_left, report->trigger_right);

	/* OFW setting 62 trigger_mode (6AE90BC5 @0x4549c): Steam Input switches to
	 * 1 = millivolts or 2 = raw SAADC counts and applies its own calibration;
	 * the digital click is not reported in those modes. */
	int16_t mode = 0;

	(void)ibex_setting_get(TRIGGER_MODE_SETTING, &mode);
	trigger_mode_dbg = (uint8_t)mode;
	if(mode == 1 || mode == 2)
	{
#if CONFIG_IBEX_TRIGGER_LOCK_HOST_MODE
		/*
		 * Steam asked for raw values so it can apply its own trigger math. Give it
		 * the voltage that corresponds to sc26re's own result instead (rest
		 * auto-zero, filter, dead zone), on the same calibration records Steam
		 * reads, so Steam's math lands exactly where ours did.
		 */
		const struct trigger_calibration *cals[2] = { &calibration.trigger_left,
		                                              &calibration.trigger_right };
		int16_t outs[2] = { report->trigger_left, report->trigger_right };

		for(int i = 0; i < 2; ++i)
		{
			int32_t e_idle;
			int32_t e_press;
			int32_t mv;

			if(!trigger_endpoints(i, cals[i], &e_idle, &e_press))
			{
				mv = averaged[i ? ANALOG_TRIGGER_RIGHT : ANALOG_TRIGGER_LEFT];
			}
			else
			{
				mv = e_idle + (int32_t)(((int64_t)outs[i] * (e_press - e_idle)) / INT16_MAX);
			}
			if(mode == 2 && host_mv[i] > 0)
			{
				mv = (int32_t)(((int64_t)mv * host_cnt[i]) / host_mv[i]);
			}
			outs[i] = (int16_t)CLAMP(mv, 0, INT16_MAX);
		}
		report->trigger_left = outs[0];
		report->trigger_right = outs[1];
#else
		ARG_UNUSED(host_cnt);
		ARG_UNUSED(host_mv);
		report->trigger_left = (int16_t)CLAMP(
		    mode == 1 ? averaged[ANALOG_TRIGGER_LEFT] : trigger_raw[0], 0, INT16_MAX);
		report->trigger_right = (int16_t)CLAMP(
		    mode == 1 ? averaged[ANALOG_TRIGGER_RIGHT] : trigger_raw[1], 0, INT16_MAX);
#endif
		trigger_left_click_latched = false;
		trigger_right_click_latched = false;
		return 0;
	}
	if(trigger_click_active(report->trigger_left, &trigger_left_click_latched))
	{
		report->buttons |= BIT(CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK);
	}
	if(trigger_click_active(report->trigger_right, &trigger_right_click_latched))
	{
		report->buttons |= BIT(CONTROLLER_BUTTON_RIGHT_TRIGGER_CLICK);
	}

	return 0;
}
