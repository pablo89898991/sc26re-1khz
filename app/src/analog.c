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
	report->gyro_quat_z = (int16_t)((side << 8) | ((trg_dbg_cal[side].inverted & 0xf) << 4) |
	                                (trigger_dir_decided[side] ? 8 : 0) | (trg_dbg_cal[side].type & 7));
}
#endif

/* Trigger step calibration (port of OFW 6AE90BC5 get_id_set_trigger_step_cal). */
#define TRIGGER_CAL_MIN_SPAN_MV 100
static struct k_spinlock trigger_cal_lock;
static uint8_t trigger_cal_stage;
static bool trigger_cal_seen[2][2]; /* [trigger][stage 1/2] */
/* [trigger][0] = normal record, [trigger][1] = inverted record, as in OFW */
static struct trigger_calibration trigger_cal_candidate[2][2];
/* sum of stage-1 (released) samples, used to reject a calibration that would read pressed at rest */
static int64_t trigger_cal_rest_sum[2];
static uint32_t trigger_cal_rest_n[2];

static void trigger_cal_sample(const int32_t value[2])
{
	k_spinlock_key_t key = k_spin_lock(&trigger_cal_lock);
	uint8_t stage = trigger_cal_stage;

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

	if(stage > 3)
	{
		return 0;
	}

	key = k_spin_lock(&trigger_cal_lock);
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
	for(size_t channel = 0; channel < ANALOG_CHANNEL_COUNT; ++channel)
	{
		averaged[channel] /= (int32_t)sample_count;
		err = adc_raw_to_millivolts_dt(&analog_channels[channel], &averaged[channel]);
		if(err)
		{
			LOG_WRN("SAADC channel %u raw-to-mV conversion failed: %d", channel, err);
			return err;
		}
	}

	struct analog_calibration_snapshot calibration;

	if(trigger_cal_stage == 1 || trigger_cal_stage == 2)
	{
		trigger_cal_sample((const int32_t[2]){averaged[ANALOG_TRIGGER_LEFT],
		                                      averaged[ANALOG_TRIGGER_RIGHT]});
	}
	calibration_analog_snapshot(&calibration);
	trigger_apply_direction(0, averaged[ANALOG_TRIGGER_LEFT], &calibration.trigger_left);
	trigger_apply_direction(1, averaged[ANALOG_TRIGGER_RIGHT], &calibration.trigger_right);
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
