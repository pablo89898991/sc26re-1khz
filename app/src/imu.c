/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/lsm6dsv16x.h>
#include <zephyr/dt-bindings/sensor/lsm6dsvxxx.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "haptics.h"
#include "controller.h"
#if CONFIG_IBEX_IMU_DEBUG_IN_QUAT
#include <zephyr/drivers/sensor_clock.h>
#endif
#include <hal/nrf_twim.h>
#include "ibex_settings_registry.h"
#include "imu.h"
#include "sc26re_cfg.h"
#include "calibration.h"
#include "imu_bias.h"
#include "gyro_mouse.h"
#include "sdl/controller_constants.h"

LOG_MODULE_REGISTER(imu);

#if CONFIG_IBEX_ACCELEROMETER && DT_NODE_HAS_STATUS(DT_ALIAS(accel0), okay)
#define IMU_HAS_DEVICE 1
static const struct device *const imu_dev = DEVICE_DT_GET(DT_ALIAS(accel0));
#else
#define IMU_HAS_DEVICE 0
#endif

#define IMU_ACCEL_UG_PER_REPORT_UNIT 61
#define IMU_GYRO_REPORT_SCALE_DIVISOR 6103
#define IMU_DEGREES_E5_PER_RADIAN 5729578.0f
#define IMU_QUATERNION_REPORT_SCALE INT16_MAX
#define IMU_SFLP_BIAS_UPDATE_INTERVAL_US USEC_PER_SEC
#define IMU_STREAM_THREAD_STACK_SIZE 2048
/* Above haptics/puck/ESB (8-10): per-batch work is now short, and a late wake-up
 * directly delays the next FIFO read. */
#define IMU_STREAM_THREAD_PRIORITY 6
#define IMU_STREAM_CQE_COUNT 16
#define IMU_STREAM_BUFFER_COUNT 8
#define IMU_STREAM_BUFFER_SIZE 512
#define IMU_STREAM_RETRY_INITIAL_MS 10U
#define IMU_STREAM_RETRY_MAX_MS 1000U
#define IMU_STREAM_RESTART_TIMEOUT_MS 1000U

#define IMU_MODE_PATH "settings/sensors/imu/mode"
#define IMU_MOUNTING_MATRIX_PATH "settings/sensors/imu/mounting_matrix"
#define IMU_GYRO_DZ_THRESHOLD_PATH "settings/sensors/imu/gyro_dz_threshold"
#define IMU_USE_BIAS_PATH "settings/sensors/imu/use_bias"
#define IMU_GYRO_THRESHOLD_PATH "settings/sensors/imu/gyro_threshold"
#define IMU_GYRO_BIAS_PATH "cal/sensors/gyroscope/bias"

static int8_t mounting_matrix[9] = {
	/* clang-format off */
	0, 1, 0,
	-1, 0, 0,
	0, 0, 1,
	/* clang-format on */
};
static int8_t staged_mounting_matrix[9];
static bool mounting_matrix_dirty;

static uint16_t imu_mode;

static int32_t gyro_dz_threshold;
static int32_t staged_gyro_dz_threshold;
static bool gyro_dz_threshold_dirty;
static bool use_gyro_bias = true;
static uint8_t gyro_threshold = 100;
static uint8_t staged_use_gyro_bias = 1;
static uint8_t staged_gyro_threshold = 100;
static bool use_gyro_bias_dirty;
static bool gyro_threshold_dirty;

static float gyro_bias[3];
static float staged_gyro_bias[3];
static float gyro_bias_candidate[3];
static void gyro_cal_sample(const float gyro[3]);

/*
 * Resting band measurement for the full gyro calibration: min/max of the
 * report's gyro axes (after smoothing, dead zones bypassed) for a while.
 * 0 idle, 1 running, 2 done, 3 moved.
 */
static atomic_t band_meas_active;
static atomic_t band_meas_state;
static int32_t band_meas_min[3];
static int32_t band_meas_max[3];
static uint32_t band_meas_frames;
static uint32_t band_meas_target;
#define BAND_MEAS_MAX_SPREAD 82 /* report units, ~5 deg/s: more means it was moved */

static void band_meas_sample(size_t i, int16_t v)
{
	if(band_meas_frames == 0)
	{
		band_meas_min[i] = v;
		band_meas_max[i] = v;
	}
	band_meas_min[i] = MIN(band_meas_min[i], v);
	band_meas_max[i] = MAX(band_meas_max[i], v);
}

static void band_meas_frame_done(void)
{
	if(!atomic_get(&band_meas_active))
	{
		return;
	}
	if(++band_meas_frames < band_meas_target)
	{
		return;
	}
	atomic_set(&band_meas_active, 0);
	for(size_t i = 0; i < 3; ++i)
	{
		if(band_meas_max[i] - band_meas_min[i] > BAND_MEAS_MAX_SPREAD)
		{
			atomic_set(&band_meas_state, 3);
			return;
		}
	}
	atomic_set(&band_meas_state, 2);
}

void imu_band_measure_start(uint32_t samples)
{
	band_meas_frames = 0;
	band_meas_target = MAX(samples, 100U);
	atomic_set(&band_meas_state, 1);
	atomic_set(&band_meas_active, 1);
}

uint8_t imu_band_measure_state(void)
{
	return (uint8_t)atomic_get(&band_meas_state);
}

/* result in 0.01 deg/s, report axes, lo <= 0 <= hi, widened by margin */
void imu_band_measure_result(int16_t lo[3], int16_t hi[3], int16_t margin)
{
	for(size_t i = 0; i < 3; ++i)
	{
		/* 1 report unit = 6.103 hundredths of a deg/s */
		int32_t l = (band_meas_min[i] * 6103 - 999) / 1000 - margin;
		int32_t h = (band_meas_max[i] * 6103 + 999) / 1000 + margin;

		lo[i] = (int16_t)CLAMP(l, -2000, 0);
		hi[i] = (int16_t)CLAMP(h, 0, 2000);
	}
}

static atomic_t full_cal_request;
static atomic_t full_cal_step;

void imu_request_full_cal(void)
{
	atomic_set(&full_cal_request, 1);
}

bool imu_take_full_cal_request(void)
{
	return atomic_cas(&full_cal_request, 1, 0);
}

void imu_set_full_cal_step(uint8_t step)
{
	atomic_set(&full_cal_step, step);
}

uint8_t imu_full_cal_step(void)
{
	return (uint8_t)atomic_get(&full_cal_step);
}
static bool gyro_bias_dirty;

static int64_t last_sflp_bias_update_us;
static struct controller_report cached_imu_report;
static struct k_spinlock cached_imu_report_lock;
#if IMU_HAS_DEVICE
static atomic_t imu_motion_trigger_enabled;
static atomic_t imu_stream_active;
static struct
{
	struct k_spinlock lock;
	uint32_t requested_generation;
	uint32_t completed_generation;
	int result;
} imu_stream_restart;
static float latest_sflp_bias[3];
static bool latest_sflp_bias_valid;
#endif

K_MUTEX_DEFINE(imu_io_lock);
K_MUTEX_DEFINE(imu_settings_mutex);

#if IMU_HAS_DEVICE
SENSOR_DT_STREAM_IODEV(imu_stream_iodev, DT_ALIAS(accel0),
                       { SENSOR_TRIG_FIFO_WATERMARK, SENSOR_STREAM_DATA_INCLUDE });
RTIO_DEFINE_WITH_MEMPOOL(imu_stream_rtio, 1, IMU_STREAM_CQE_COUNT, IMU_STREAM_BUFFER_COUNT,
                         IMU_STREAM_BUFFER_SIZE, sizeof(void *));
K_SEM_DEFINE(imu_stream_start_sem, 0, 1);
K_SEM_DEFINE(imu_stream_restart_done_sem, 0, 1);
K_MUTEX_DEFINE(imu_bias_program_lock);
K_THREAD_STACK_DEFINE(imu_stream_stack, IMU_STREAM_THREAD_STACK_SIZE);
static struct k_thread imu_stream_thread;
static void imu_stream_thread_main(void *arg1, void *arg2, void *arg3);
static void imu_fast_thread_main(void *arg1, void *arg2, void *arg3);
#define IMU_FAST_THREAD_STACK_SIZE 2048
/* above the FIFO stream thread: it reads the sample the next report carries */
#define IMU_FAST_THREAD_PRIORITY 5
K_THREAD_STACK_DEFINE(imu_fast_stack, IMU_FAST_THREAD_STACK_SIZE);
static struct k_thread imu_fast_thread;
#endif

struct imu_settings_load_target
{
	void *value;
	size_t value_size;
	bool loaded;
};

#if IMU_HAS_DEVICE
static float q31_to_float(q31_t value, int8_t shift)
{
	return ldexpf((float)value, shift - 31);
}

static int16_t clamp_i16_int(int32_t value)
{
	return (int16_t)CLAMP(value, INT16_MIN, INT16_MAX);
}

static int16_t accel_axis_to_report(float ms2)
{
	float report_units =
	    ms2 * 1000000.0f / ((float)SENSOR_G / 1000000.0f * IMU_ACCEL_UG_PER_REPORT_UNIT);

	return clamp_i16_int((int32_t)report_units);
}

static int16_t gyro_axis_to_report(float rad_s)
{
	int32_t degrees_e5 = (int32_t)(rad_s * IMU_DEGREES_E5_PER_RADIAN);

	return clamp_i16_int(degrees_e5 / IMU_GYRO_REPORT_SCALE_DIVISOR);
}

static int16_t quaternion_component_to_report(float component)
{
	return clamp_i16_int((int32_t)(component * IMU_QUATERNION_REPORT_SCALE));
}

static void apply_orientation_i16(int16_t xyz[3])
{
	int16_t in[3] = { xyz[0], xyz[1], xyz[2] };

	for(size_t row = 0; row < 3; ++row)
	{
		int32_t value = 0;

		for(size_t col = 0; col < 3; ++col)
		{
			value += mounting_matrix[row * 3 + col] * in[col];
		}
		xyz[row] = clamp_i16_int(value);
	}
}
#endif

static void fill_identity_quaternion_report(struct controller_report *report)
{
	report->gyro_quat_w = INT16_MAX;
	report->gyro_quat_x = 0;
	report->gyro_quat_y = 0;
	report->gyro_quat_z = 0;
}

#if IMU_HAS_DEVICE
static void fill_quaternion_report(struct controller_report *report, const float value[4])
{
	int16_t xyz[3] = {
		quaternion_component_to_report(value[0]),
		quaternion_component_to_report(value[1]),
		quaternion_component_to_report(value[2]),
	};

	apply_orientation_i16(xyz);
	report->gyro_quat_w = quaternion_component_to_report(value[3]);
	report->gyro_quat_x = xyz[0];
	report->gyro_quat_y = xyz[1];
	report->gyro_quat_z = xyz[2];
}
#endif

static void copy_imu_report_fields(struct controller_report *dst,
                                   const struct controller_report *src)
{
	dst->accel_x = src->accel_x;
	dst->accel_y = src->accel_y;
	dst->accel_z = src->accel_z;
	dst->gyro_x = src->gyro_x;
	dst->gyro_y = src->gyro_y;
	dst->gyro_z = src->gyro_z;
	dst->gyro_quat_w = src->gyro_quat_w;
	dst->gyro_quat_x = src->gyro_quat_x;
	dst->gyro_quat_y = src->gyro_quat_y;
	dst->gyro_quat_z = src->gyro_quat_z;
	dst->imu_timestamp_us = src->imu_timestamp_us;
}

#if IMU_HAS_DEVICE
static void store_cached_imu_report(const struct controller_report *report)
{
	k_spinlock_key_t key = k_spin_lock(&cached_imu_report_lock);

	copy_imu_report_fields(&cached_imu_report, report);
	k_spin_unlock(&cached_imu_report_lock, key);
}
#endif

static void load_cached_imu_report(struct controller_report *report)
{
	k_spinlock_key_t key = k_spin_lock(&cached_imu_report_lock);

	copy_imu_report_fields(report, &cached_imu_report);
	k_spin_unlock(&cached_imu_report_lock, key);
}

/*
 * While the gyro mouse is on, nothing else gets the motion data: the pad
 * report carries no gyro/accel and an identity quaternion (the timestamp
 * still moves, so report freshness stays visible).
 */
static void hide_motion_if_mouse(struct controller_report *report)
{
	if(!gyro_mouse_enabled())
	{
		return;
	}
	report->gyro_x = report->gyro_y = report->gyro_z = 0;
	report->accel_x = report->accel_y = report->accel_z = 0;
	fill_identity_quaternion_report(report);
}

#if IMU_HAS_DEVICE
static void update_bias_candidate_from_sflp(const float value[3])
{
	int64_t now_us = k_ticks_to_us_floor64(k_uptime_ticks());

	if(last_sflp_bias_update_us != 0 &&
	   now_us - last_sflp_bias_update_us < IMU_SFLP_BIAS_UPDATE_INTERVAL_US)
	{
		return;
	}

	for(size_t i = 0; i < 3; ++i)
	{
		gyro_bias_candidate[i] = value[i];
	}
	last_sflp_bias_update_us = now_us;
}
#endif

#if IMU_HAS_DEVICE
static uint32_t request_stream_restart(void)
{
	k_spinlock_key_t key = k_spin_lock(&imu_stream_restart.lock);

	++imu_stream_restart.requested_generation;
	if(imu_stream_restart.requested_generation == 0U)
	{
		++imu_stream_restart.requested_generation;
	}
	uint32_t generation = imu_stream_restart.requested_generation;

	k_spin_unlock(&imu_stream_restart.lock, key);
	return generation;
}

static uint32_t pending_stream_restart(void)
{
	k_spinlock_key_t key = k_spin_lock(&imu_stream_restart.lock);
	uint32_t generation =
	    imu_stream_restart.requested_generation != imu_stream_restart.completed_generation
	        ? imu_stream_restart.requested_generation
	        : 0U;

	k_spin_unlock(&imu_stream_restart.lock, key);
	return generation;
}

static void complete_stream_restart(uint32_t generation, int result)
{
	k_spinlock_key_t key = k_spin_lock(&imu_stream_restart.lock);

	imu_stream_restart.completed_generation = generation;
	imu_stream_restart.result = result;
	k_spin_unlock(&imu_stream_restart.lock, key);
	k_sem_give(&imu_stream_restart_done_sem);
}

static int wait_for_stream_restart(uint32_t generation)
{
	k_timepoint_t deadline = sys_timepoint_calc(K_MSEC(IMU_STREAM_RESTART_TIMEOUT_MS));

	for(;;)
	{
		k_spinlock_key_t key = k_spin_lock(&imu_stream_restart.lock);

		if(imu_stream_restart.completed_generation == generation)
		{
			int result = imu_stream_restart.result;

			k_spin_unlock(&imu_stream_restart.lock, key);
			return result;
		}
		k_spin_unlock(&imu_stream_restart.lock, key);

		k_timeout_t timeout = sys_timepoint_timeout(deadline);

		if(K_TIMEOUT_EQ(timeout, K_NO_WAIT) ||
		   k_sem_take(&imu_stream_restart_done_sem, timeout) != 0)
		{
			return -ETIMEDOUT;
		}
	}
}
#endif

static int program_gyro_bias(void)
{
#if IMU_HAS_DEVICE
	struct sensor_value bias[3];
	uint32_t restart_generation = 0U;
	int err;

	if(!device_is_ready(imu_dev))
	{
		return 0;
	}

	k_mutex_lock(&imu_bias_program_lock, K_FOREVER);
	k_mutex_lock(&imu_io_lock, K_FOREVER);
	for(size_t i = 0; i < 3; ++i)
	{
		(void)sensor_value_from_float(&bias[i], gyro_bias[i]);
	}
	err = sensor_attr_set(imu_dev, SENSOR_CHAN_GBIAS_XYZ, SENSOR_ATTR_OFFSET, bias);
	if(!err && atomic_get(&imu_stream_active) != 0)
	{
		restart_generation = request_stream_restart();
	}
	k_mutex_unlock(&imu_io_lock);

	if(restart_generation != 0U)
	{
		err = wait_for_stream_restart(restart_generation);
	}
	k_mutex_unlock(&imu_bias_program_lock);

	return err;
#else
	return 0;
#endif
}

static int program_gyro_threshold(void)
{
#if IMU_HAS_DEVICE
	struct sensor_value threshold = { 0 };
	int err;

	if(!device_is_ready(imu_dev))
	{
		return 0;
	}

	k_mutex_lock(&imu_bias_program_lock, K_FOREVER);
	k_mutex_lock(&imu_io_lock, K_FOREVER);
	threshold.val1 = gyro_threshold;
	/* The driver serializes this update with FIFO I/O without resetting SFLP. */
	err = sensor_attr_set(imu_dev, SENSOR_CHAN_GYRO_XYZ,
	                      SENSOR_ATTR_LSM6DSV16X_GYRO_CALIBRATION_THRESHOLD, &threshold);
	k_mutex_unlock(&imu_io_lock);
	k_mutex_unlock(&imu_bias_program_lock);
	return err;
#else
	return 0;
#endif
}

static bool imu_kept_on;

static void set_motion_trigger_enabled(bool enabled)
{
#if IMU_HAS_DEVICE
	/* the gyro mouse keeps the IMU running even when the host switched it off */
	enabled = enabled || imu_kept_on;
	atomic_val_t previous = atomic_set(&imu_motion_trigger_enabled, enabled ? 1 : 0);

	if(enabled && previous == 0)
	{
		k_sem_give(&imu_stream_start_sem);
	}
#else
	ARG_UNUSED(enabled);
#endif
}

/*
 * Keep the IMU streaming while a calibration runs even if no host switched it
 * on (button-chord calibration without Steam). Restores the host's mode after.
 */
static void apply_mode(uint16_t mode);
static bool imu_forced_on;

void imu_force_on(bool on)
{
	int16_t host_mode = SETTING_GYRO_MODE_OFF;

	(void)ibex_setting_get(IBEX_SETTING_IMU_MODE, &host_mode);
	if(on && !imu_forced_on && host_mode == SETTING_GYRO_MODE_OFF)
	{
		imu_forced_on = true;
		apply_mode(SETTING_GYRO_MODE_SEND_RAW_ACCEL | SETTING_GYRO_MODE_SEND_RAW_GYRO);
	}
	else if(!on && imu_forced_on)
	{
		imu_forced_on = false;
		apply_mode((uint16_t)host_mode);
	}
}

void imu_keep_on(bool on)
{
	if(on == imu_kept_on)
	{
		return;
	}
	imu_kept_on = on;
	set_motion_trigger_enabled(*(volatile uint16_t *)&imu_mode != SETTING_GYRO_MODE_OFF);
}

void imu_reset(void)
{
	k_mutex_lock(&imu_io_lock, K_FOREVER);
	last_sflp_bias_update_us = 0;
	k_mutex_unlock(&imu_io_lock);
	(void)program_gyro_threshold();
	(void)program_gyro_bias();
}

static void apply_mode(uint16_t mode)
{
	bool was_enabled;
	bool enabled = mode != SETTING_GYRO_MODE_OFF;
	uint16_t old_mode;

	k_mutex_lock(&imu_io_lock, K_FOREVER);
	old_mode = imu_mode;
	was_enabled = old_mode != SETTING_GYRO_MODE_OFF;
	imu_mode = mode;
	k_mutex_unlock(&imu_io_lock);
	if(enabled && (!was_enabled || old_mode != mode))
	{
		imu_reset();
	}
	if(was_enabled != enabled)
	{
		set_motion_trigger_enabled(enabled);
	}
}

static void setting_changed(uint8_t id, int16_t value)
{
	if(id == IBEX_SETTING_IMU_MODE)
	{
		apply_mode((uint16_t)value);
	}
	else if(id == IBEX_SETTING_IMU_USE_BIAS)
	{
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		use_gyro_bias = value != 0;
		k_mutex_unlock(&imu_io_lock);
	}
	else if(id == IBEX_SETTING_IMU_GYRO_THRESHOLD)
	{
		int err;

		k_mutex_lock(&imu_io_lock, K_FOREVER);
		gyro_threshold = (uint8_t)value;
		k_mutex_unlock(&imu_io_lock);
		err = program_gyro_threshold();
		if(err)
		{
			LOG_WRN("failed to program gyro calibration threshold: %d", err);
		}
	}
}

static int load_exact_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
                         void *param)
{
	struct imu_settings_load_target *target = param;
	ssize_t read_len;

	if(key != NULL && key[0] != '\0')
	{
		return 0;
	}
	if(len != target->value_size)
	{
		return -EINVAL;
	}

	read_len = read_cb(cb_arg, target->value, target->value_size);
	if(read_len < 0)
	{
		return read_len;
	}
	if((size_t)read_len != target->value_size)
	{
		return -EINVAL;
	}

	target->loaded = true;
	return 1;
}

static bool load_setting_exact(const char *path, void *value, size_t value_size)
{
	struct imu_settings_load_target target = {
		.value = value,
		.value_size = value_size,
	};

	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		return false;
	}

	(void)settings_load_subtree_direct(path, load_exact_cb, &target);
	return target.loaded;
}

static void load_persisted_settings(void)
{
	int16_t loaded_mode;
	int32_t loaded_threshold;
	uint8_t loaded_use_bias;
	uint8_t loaded_gyro_threshold;
	float loaded_bias[3];
	int8_t loaded_matrix[9];

	if(load_setting_exact(IMU_MOUNTING_MATRIX_PATH, loaded_matrix, sizeof(loaded_matrix)))
	{
		memcpy(mounting_matrix, loaded_matrix, sizeof(mounting_matrix));
	}
	if(load_setting_exact(IMU_GYRO_DZ_THRESHOLD_PATH, &loaded_threshold, sizeof(loaded_threshold)))
	{
		gyro_dz_threshold = loaded_threshold;
	}
	if(load_setting_exact(IMU_USE_BIAS_PATH, &loaded_use_bias, sizeof(loaded_use_bias)))
	{
		(void)ibex_setting_set(IBEX_SETTING_IMU_USE_BIAS, loaded_use_bias);
	}
	if(load_setting_exact(IMU_GYRO_THRESHOLD_PATH, &loaded_gyro_threshold,
	                      sizeof(loaded_gyro_threshold)))
	{
		(void)ibex_setting_set(IBEX_SETTING_IMU_GYRO_THRESHOLD, loaded_gyro_threshold);
	}
	if(load_setting_exact(IMU_GYRO_BIAS_PATH, loaded_bias, sizeof(loaded_bias)))
	{
		if(imu_gyro_bias_valid(loaded_bias))
		{
			memcpy(gyro_bias, loaded_bias, sizeof(gyro_bias));
			memcpy(gyro_bias_candidate, loaded_bias, sizeof(gyro_bias_candidate));
		}
		else
		{
			LOG_WRN("ignoring invalid persisted gyro bias");
		}
	}
	else
	{
		memcpy(gyro_bias_candidate, gyro_bias, sizeof(gyro_bias_candidate));
	}
	if(load_setting_exact(IMU_MODE_PATH, &loaded_mode, sizeof(loaded_mode)))
	{
		(void)ibex_setting_set(IBEX_SETTING_IMU_MODE, loaded_mode);
	}

	memcpy(staged_mounting_matrix, mounting_matrix, sizeof(staged_mounting_matrix));
	staged_gyro_dz_threshold = gyro_dz_threshold;
	{
		int16_t value;

		(void)ibex_setting_get(IBEX_SETTING_IMU_USE_BIAS, &value);
		staged_use_gyro_bias = (uint8_t)value;
		(void)ibex_setting_get(IBEX_SETTING_IMU_GYRO_THRESHOLD, &value);
		staged_gyro_threshold = (uint8_t)value;
	}
	memcpy(staged_gyro_bias, gyro_bias, sizeof(staged_gyro_bias));
}

int imu_init(void)
{
	int err;

	fill_identity_quaternion_report(&cached_imu_report);

	load_persisted_settings();

	err = ibex_settings_register_callback(setting_changed);
	if(err)
	{
		LOG_WRN("failed to register IMU setting callback: %d", err);
	}

#if IMU_HAS_DEVICE
	if(!device_is_ready(imu_dev))
	{
		LOG_WRN("IMU is not ready (initialized=%u init_res=%u), reports will contain zeroes",
		        imu_dev->state->initialized, imu_dev->state->init_res);
	}
	else
	{
#if CONFIG_IBEX_IMU_I2C_FREQ_KHZ != 400 && DT_NODE_HAS_COMPAT(DT_BUS(DT_ALIAS(accel0)), nordic_nrf_twim)
		{
			/*
			 * Undocumented on nRF52833 (TWIM lists 100/250/400 kHz): FREQUENCY is a
			 * fractional rate word (value / 2^32 * 16 MHz). The bus is idle here.
			 */
			NRF_TWIM_Type *twim = (NRF_TWIM_Type *)DT_REG_ADDR(DT_BUS(DT_ALIAS(accel0)));
			twim->FREQUENCY = (uint32_t)(((uint64_t)CONFIG_IBEX_IMU_I2C_FREQ_KHZ << 32) / 16000U);
			LOG_INF("IMU I2C bus forced to %d kHz", CONFIG_IBEX_IMU_I2C_FREQ_KHZ);
		}
#endif
		k_thread_create(&imu_stream_thread, imu_stream_stack,
		                K_THREAD_STACK_SIZEOF(imu_stream_stack), imu_stream_thread_main, NULL, NULL,
		                NULL, K_PRIO_PREEMPT(IMU_STREAM_THREAD_PRIORITY), K_FP_REGS, K_NO_WAIT);
		(void)k_thread_name_set(&imu_stream_thread, "imu_stream");
		k_thread_create(&imu_fast_thread, imu_fast_stack, K_THREAD_STACK_SIZEOF(imu_fast_stack),
		                imu_fast_thread_main, NULL, NULL, NULL,
		                K_PRIO_PREEMPT(IMU_FAST_THREAD_PRIORITY), K_FP_REGS, K_NO_WAIT);
		(void)k_thread_name_set(&imu_fast_thread, "imu_fast");
	}
#else
	LOG_WRN("no accel0 device-tree alias, reports will contain zeroes");
#endif

	return 0;
}

bool imu_ready(void)
{
#if IMU_HAS_DEVICE
	return device_is_ready(imu_dev);
#else
	return false;
#endif
}

#if IMU_HAS_DEVICE
/*
 * One FIFO batch can hold several frames per channel; only the newest one is used.
 * Decode them in chunks with a single sensor_decode() call per chunk instead of one
 * call per frame: each call rescans the batch, so per-frame decoding was O(n^2) and
 * dominated the IMU thread's CPU time at high batch rates.
 */
#define IMU_DECODE_CHUNK 8

#if CONFIG_IBEX_IMU_DEBUG_IN_QUAT
/*
 * Diagnostics build: the quaternion fields of the state report carry averages over
 * 256 FIFO batches instead of orientation:
 *   w = IRQ->processing latency (us), x = build time (us),
 *   y = batch period (us),            z = gyro frames per batch * 100
 */
static uint16_t dbg_gyro_frames;
static uint32_t dbg_acc_lat, dbg_acc_build, dbg_acc_period, dbg_acc_frames, dbg_n, dbg_last_cyc;
static int16_t dbg_out[4];
static uint32_t dbg_scan_acc, dbg_scan_n;
static int16_t dbg_scan_avg;

void imu_debug_note_scan_us(uint32_t us)
{
	dbg_scan_acc += us;
	if(++dbg_scan_n == 256U)
	{
		dbg_scan_avg = (int16_t)(dbg_scan_acc / 256U);
		dbg_scan_acc = dbg_scan_n = 0;
	}
}
#endif

static int decode_latest_three_axis_ts(const struct sensor_decoder_api *decoder,
                                       const uint8_t *buf, enum sensor_channel channel,
                                       float value[3], uint64_t *timestamp_ns)
{
	struct sensor_chan_spec chan_spec = { channel, 0 };
	struct sensor_decode_context ctx = SENSOR_DECODE_CONTEXT_INIT(decoder, buf, channel, 0);
	struct
	{
		struct sensor_three_axis_data data;
		struct sensor_three_axis_sample_data more[IMU_DECODE_CHUNK - 1];
	} out;
	uint16_t frame_count;
	int err;
	int n = 0;

	err = decoder->get_frame_count(buf, chan_spec, &frame_count);
	if(err)
	{
		return err;
	}
	if(frame_count == 0)
	{
		return -ENODATA;
	}

#if CONFIG_IBEX_IMU_DEBUG_IN_QUAT
	if(channel == SENSOR_CHAN_GYRO_XYZ)
	{
		dbg_gyro_frames = frame_count;
	}
#endif
	while(frame_count > 0)
	{
		n = sensor_decode(&ctx, &out.data, MIN(frame_count, IMU_DECODE_CHUNK));
		if(n <= 0)
		{
			return n < 0 ? n : -EIO;
		}
		frame_count -= MIN(frame_count, (uint16_t)n);
	}
	for(size_t axis = 0; axis < 3; ++axis)
	{
		value[axis] = q31_to_float(out.data.readings[n - 1].values[axis], out.data.shift);
	}
	if(timestamp_ns != NULL)
	{
		*timestamp_ns =
		    out.data.header.base_timestamp_ns + out.data.readings[n - 1].timestamp_delta;
	}

	return 0;
}

static int decode_latest_three_axis(const struct sensor_decoder_api *decoder, const uint8_t *buf,
                                    enum sensor_channel channel, float value[3])
{
	return decode_latest_three_axis_ts(decoder, buf, channel, value, NULL);
}

static int decode_latest_quaternion(const struct sensor_decoder_api *decoder, const uint8_t *buf,
                                    float value[4])
{
	struct sensor_chan_spec chan_spec = { SENSOR_CHAN_GAME_ROTATION_VECTOR, 0 };
	struct sensor_decode_context ctx =
	    SENSOR_DECODE_CONTEXT_INIT(decoder, buf, SENSOR_CHAN_GAME_ROTATION_VECTOR, 0);
	struct
	{
		struct sensor_game_rotation_vector_data data;
		struct sensor_game_rotation_vector_sample_data more[IMU_DECODE_CHUNK - 1];
	} out;
	uint16_t frame_count;
	int err;
	int n = 0;

	err = decoder->get_frame_count(buf, chan_spec, &frame_count);
	if(err)
	{
		return err;
	}
	if(frame_count == 0)
	{
		return -ENODATA;
	}

	while(frame_count > 0)
	{
		n = sensor_decode(&ctx, &out.data, MIN(frame_count, IMU_DECODE_CHUNK));
		if(n <= 0)
		{
			return n < 0 ? n : -EIO;
		}
		frame_count -= MIN(frame_count, (uint16_t)n);
	}
	for(size_t component = 0; component < 4; ++component)
	{
		value[component] = q31_to_float(out.data.readings[n - 1].values[component], out.data.shift);
	}

	return 0;
}

static int decode_latest_fifo_timestamp(const struct sensor_decoder_api *decoder,
                                        const uint8_t *buf, uint32_t *timestamp_us)
{
	struct sensor_chan_spec chan_spec = { SENSOR_CHAN_LSM6DSV16X_FIFO_TIMESTAMP, 0 };
	struct sensor_decode_context ctx =
	    SENSOR_DECODE_CONTEXT_INIT(decoder, buf, SENSOR_CHAN_LSM6DSV16X_FIFO_TIMESTAMP, 0);
	struct
	{
		struct sensor_uint64_data data;
		struct sensor_uint64_sample_data more[IMU_DECODE_CHUNK - 1];
	} out;
	uint16_t frame_count;
	int err;
	int n = 0;

	err = decoder->get_frame_count(buf, chan_spec, &frame_count);
	if(err)
	{
		return err;
	}
	if(frame_count == 0)
	{
		return -ENODATA;
	}

	while(frame_count > 0)
	{
		n = sensor_decode(&ctx, &out.data, MIN(frame_count, IMU_DECODE_CHUNK));
		if(n <= 0)
		{
			return n < 0 ? n : -EIO;
		}
		frame_count -= MIN(frame_count, (uint16_t)n);
	}
	*timestamp_us = (uint32_t)(out.data.readings[n - 1].value * LSM6DSV16X_FIFO_TIMESTAMP_TICK_US);

	return 0;
}

#if CONFIG_IBEX_DROP_SCREAM
/*
 * OFW 6AE90BC5 enables the LSM6DSV16X free-fall detector (156 mg threshold, FF_DUR 63) and,
 * on a fall, runs 0x45468: random byte < 48 -> Wilhelm scream (script 16), < 128 -> meow
 * (script 17), otherwise nothing. Here the same is done in software on the accel stream:
 * |a| below 156 mg for IBEX_DROP_FREE_FALL_MS counts as one fall.
 */
#define IMU_FREE_FALL_MS2 (0.156f * 9.80665f)
#define IMU_FREE_FALL_REARM_MS2 (0.5f * 9.80665f)
#define IMU_FREE_FALL_COOLDOWN_MS 2000

static void imu_free_fall_update(const float accel[3])
{
	static int64_t below_since_ms = -1;
	static int64_t last_fall_ms = -IMU_FREE_FALL_COOLDOWN_MS;
	static bool fired;
	float mag2 = accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2];
	int64_t now = k_uptime_get();

	if(mag2 < IMU_FREE_FALL_MS2 * IMU_FREE_FALL_MS2)
	{
		if(below_since_ms < 0)
		{
			below_since_ms = now;
		}
		if(!fired && now - below_since_ms >= CONFIG_IBEX_DROP_FREE_FALL_MS &&
		   now - last_fall_ms >= IMU_FREE_FALL_COOLDOWN_MS)
		{
			fired = true;
			last_fall_ms = now;
			(void)haptics_play_drop_sound();
		}
		return;
	}
	below_since_ms = -1;
	if(mag2 > IMU_FREE_FALL_REARM_MS2 * IMU_FREE_FALL_REARM_MS2)
	{
		fired = false;
	}
}
#endif

/* accel sample (m/s^2, sensor frame) -> report fields */
static void process_accel(const float accel[3], struct controller_report *report)
{
	int16_t oriented[3];

#if CONFIG_IBEX_DROP_SCREAM
	imu_free_fall_update(accel);
#endif
	oriented[0] = accel_axis_to_report(accel[0]);
	oriented[1] = accel_axis_to_report(accel[1]);
	oriented[2] = accel_axis_to_report(accel[2]);
	apply_orientation_i16(oriented);
	report->accel_x = oriented[0];
	report->accel_y = oriented[1];
	report->accel_z = oriented[2];
}

/* gyro sample (rad/s, sensor frame) -> calibration, dead zones, filters -> report fields */
static void process_gyro(const float gyro[3], struct controller_report *report)
{
	int16_t oriented[3];
	float bias[3];

	gyro_cal_sample(gyro);
	memcpy(bias, latest_sflp_bias_valid ? latest_sflp_bias : gyro_bias, sizeof(bias));
	if(!use_gyro_bias)
	{
		memset(bias, 0, sizeof(bias));
	}
	oriented[0] = gyro_axis_to_report(gyro[0] - bias[0]);
	oriented[1] = gyro_axis_to_report(gyro[1] - bias[1]);
	oriented[2] = gyro_axis_to_report(gyro[2] - bias[2]);
	apply_orientation_i16(oriented);
	{
		/* report meter overrides: dead zone (auto = Steam's) and low-pass strength */
		int16_t cfg_dz = sc26re_cfg_get(SC26RE_CFG_GYRO_DEADZONE);
		int16_t cfg_lp = sc26re_cfg_get(SC26RE_CFG_GYRO_FILTER);
		int32_t dz = cfg_dz == SC26RE_CFG_AUTO ? gyro_dz_threshold : cfg_dz;
		static int32_t gyro_lp_q8[3];
		static bool gyro_lp_primed;

		for(size_t i = 0; i < 3; ++i)
		{
			if(cfg_lp > 0)
			{
				int32_t x = (int32_t)oriented[i] << 8;

				if(!gyro_lp_primed)
				{
					gyro_lp_q8[i] = x;
				}
				gyro_lp_q8[i] += (x - gyro_lp_q8[i]) >> MIN(cfg_lp, 4);
				oriented[i] = (int16_t)CLAMP(gyro_lp_q8[i] >> 8, INT16_MIN, INT16_MAX);
			}
			if(atomic_get(&band_meas_active))
			{
				/* measuring the resting band: dead zones are bypassed */
				band_meas_sample(i, oriented[i]);
				continue;
			}
			if(-dz < oriented[i] && dz > oriented[i])
			{
				oriented[i] = 0;
			}
			{
				/* per-axis dead band from the report meter, 0.01 deg/s -> report
				 * units (+-2000 deg/s full scale: 16.384 counts per deg/s) */
				int32_t lo = sc26re_cfg_get(SC26RE_CFG_GYRO_BAND_X_LO + 2 * i);
				int32_t hi = sc26re_cfg_get(SC26RE_CFG_GYRO_BAND_X_HI + 2 * i);

				if(lo != 0 || hi != 0)
				{
					lo = (lo * 16384) / 100000;
					hi = (hi * 16384) / 100000;
					if(oriented[i] >= lo && oriented[i] <= hi)
					{
						oriented[i] = 0;
					}
					else if(sc26re_cfg_get(SC26RE_CFG_GYRO_BAND_SOFT) == 1)
					{
						/* soft: continue from 0 at the band's edge, no jump */
						oriented[i] = (int16_t)(oriented[i] > hi ? oriented[i] - hi
						                                         : oriented[i] - lo);
					}
				}
			}
		}
		gyro_lp_primed = cfg_lp > 0;
		band_meas_frame_done();
	}
	report->gyro_x = oriented[0];
	report->gyro_y = oriented[1];
	report->gyro_z = oriented[2];
}

static int build_imu_report_from_stream(const uint8_t *buf)
{
	const struct sensor_decoder_api *decoder;
	struct controller_report report = { 0 };
	float accel[3];
	float gyro[3];
	float sflp_bias[3];
	float quaternion[4];
	uint32_t timestamp_us = 0;
	bool updated = false;
	bool gyro_updated = false;
	uint64_t gyro_timestamp_ns = 0;
	bool reports_enabled;
	int err;

	err = sensor_get_decoder(imu_dev, &decoder);
	if(err)
	{
		return err;
	}
#if CONFIG_IBEX_IMU_DEBUG_IN_QUAT
	uint32_t dbg_t0 = k_cycle_get_32();
	uint64_t dbg_now_cycles = 0;

	(void)sensor_clock_get_cycles(&dbg_now_cycles);
	uint64_t dbg_now_ns = sensor_clock_cycles_to_ns(dbg_now_cycles);
#endif

	k_mutex_lock(&imu_io_lock, K_FOREVER);
	reports_enabled = atomic_get(&imu_motion_trigger_enabled) != 0;
	if(!reports_enabled)
	{
		k_mutex_unlock(&imu_io_lock);
		return 0;
	}

	load_cached_imu_report(&report);
	err = decode_latest_three_axis(decoder, buf, SENSOR_CHAN_GBIAS_XYZ, sflp_bias);
	if(err == 0)
	{
		memcpy(latest_sflp_bias, sflp_bias, sizeof(latest_sflp_bias));
		latest_sflp_bias_valid = true;
		update_bias_candidate_from_sflp(sflp_bias);
		updated = true;
	}

	err = decode_latest_three_axis(decoder, buf, SENSOR_CHAN_ACCEL_XYZ, accel);
	if(err == 0)
	{
		process_accel(accel, &report);
		updated = true;
	}

	err = decode_latest_three_axis_ts(decoder, buf, SENSOR_CHAN_GYRO_XYZ, gyro, &gyro_timestamp_ns);
	if(err == 0)
	{
		process_gyro(gyro, &report);
		updated = true;
		gyro_updated = true;
	}

	err = decode_latest_quaternion(decoder, buf, quaternion);
	if(err == 0)
	{
		fill_quaternion_report(&report, quaternion);
		updated = true;
	}
	err = decode_latest_fifo_timestamp(decoder, buf, &timestamp_us);
	if(err == 0)
	{
		report.imu_timestamp_us = timestamp_us;
	}
	else if(gyro_updated)
	{
		/* No FIFO timestamp words: use the newest gyro frame's derived time. */
		report.imu_timestamp_us = (uint32_t)(gyro_timestamp_ns / NSEC_PER_USEC);
	}
#if CONFIG_IBEX_IMU_DEBUG_IN_QUAT
	if(gyro_updated)
	{
		uint32_t t1 = k_cycle_get_32();
		uint64_t lat = dbg_now_ns > gyro_timestamp_ns ? dbg_now_ns - gyro_timestamp_ns : 0;

		dbg_acc_lat += (uint32_t)MIN(lat / NSEC_PER_USEC, 60000U);
		dbg_acc_build += k_cyc_to_us_floor32(t1 - dbg_t0);
		dbg_acc_period += dbg_last_cyc ? MIN(k_cyc_to_us_floor32(dbg_t0 - dbg_last_cyc), 60000U) : 0;
		dbg_acc_frames += dbg_gyro_frames;
		dbg_last_cyc = dbg_t0;
		if(++dbg_n == 256U)
		{
			dbg_out[0] = (int16_t)(dbg_acc_lat / 256U);
			dbg_out[1] = (int16_t)(dbg_acc_build / 256U);
			dbg_out[2] = (int16_t)(dbg_acc_period / 256U);
			dbg_out[3] = (int16_t)(dbg_acc_frames * 100U / 256U);
			(void)dbg_out[3];
			dbg_acc_lat = dbg_acc_build = dbg_acc_period = dbg_acc_frames = dbg_n = 0;
		}
	}
	report.gyro_quat_w = dbg_out[0];
	report.gyro_quat_x = dbg_out[1];
	report.gyro_quat_y = dbg_out[2];
	report.gyro_quat_z = dbg_scan_avg; /* analog+buttons scan time in the report loop (us) */
#endif
	if(updated)
	{
		store_cached_imu_report(&report);
		k_mutex_unlock(&imu_io_lock);
#if CONFIG_IBEX_IMU_WAKES_REPORT
		if(gyro_updated && (transport_usb_configured() || transport_esb_connected()))
		{
			/* Let the report loop ship each fresh gyro sample immediately. */
			hardware_signal_input_changed();
		}
#endif
		return 0;
	}
	k_mutex_unlock(&imu_io_lock);
	return -ENODATA;
}

static void imu_stream_thread_main(void *arg1, void *arg2, void *arg3)
{
	bool initial_stream_attempted = false;
	uint32_t retry_ms = IMU_STREAM_RETRY_INITIAL_MS;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for(;;)
	{
		struct rtio_sqe *stream_handle = NULL;
		uint32_t restart_generation;
		bool cancel_pending = false;
		int err;

		/*
		 * Configure the stream once even when reports start disabled. OFW removes
		 * only its report trigger, leaving the sensor and SFLP running so fusion
		 * state remains warm across a later re-enable.
		 */
		while(initial_stream_attempted &&
		      atomic_get(&imu_motion_trigger_enabled) == 0 &&
		      pending_stream_restart() == 0U)
		{
			k_sem_take(&imu_stream_start_sem, K_FOREVER);
		}

		initial_stream_attempted = true;
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		restart_generation = pending_stream_restart();
		err = sensor_stream(&imu_stream_iodev, &imu_stream_rtio, NULL, &stream_handle);
		if(!err)
		{
			atomic_set(&imu_stream_active, 1);
		}
		k_mutex_unlock(&imu_io_lock);
		if(restart_generation != 0U)
		{
			/* Report the first replacement attempt; recovery remains independent. */
			complete_stream_restart(restart_generation, err);
		}
		if(err)
		{
			LOG_WRN("failed to start IMU FIFO stream: %d", err);
			if(atomic_get(&imu_motion_trigger_enabled) == 0 && pending_stream_restart() == 0U)
			{
				retry_ms = IMU_STREAM_RETRY_INITIAL_MS;
				continue;
			}
			k_msleep(retry_ms);
			retry_ms = MIN(retry_ms * 2U, IMU_STREAM_RETRY_MAX_MS);
			continue;
		}
		for(;;)
		{
			struct rtio_cqe *cqe = rtio_cqe_consume_block(&imu_stream_rtio);
			uint8_t *buf = NULL;
			uint32_t buf_len = 0;
			int stream_result = cqe->result;
			int result = stream_result;

			if(result == 0)
			{
				result = rtio_cqe_get_mempool_buffer(&imu_stream_rtio, cqe, &buf, &buf_len);
				if(result == 0 && (buf == NULL || buf_len == 0))
				{
					result = -ENODATA;
				}
			}
			rtio_cqe_release(&imu_stream_rtio, cqe);

			if(result == 0)
			{
				err = build_imu_report_from_stream(buf);
				if(err && err != -ENODATA)
				{
					LOG_DBG("failed to decode IMU FIFO stream: %d", err);
				}
				rtio_release_buffer(&imu_stream_rtio, buf, buf_len);
				retry_ms = IMU_STREAM_RETRY_INITIAL_MS;
			}
			else if(stream_result == 0)
			{
				/* An empty success does not terminate the live multishot request. */
				LOG_DBG("IMU FIFO stream completed without data: %d", result);
			}

			if(stream_result < 0)
			{
				atomic_set(&imu_stream_active, 0);
				if(stream_result != -ECANCELED)
				{
					LOG_WRN("IMU FIFO stream stopped: %d", stream_result);
				}
				break;
			}
			if(!cancel_pending &&
			   (atomic_get(&imu_motion_trigger_enabled) == 0 || pending_stream_restart() != 0U))
			{
				/*
				 * This private RTIO has one SQE, and this thread does not reacquire it
				 * until the terminal CQE is consumed below.
				 */
				err = rtio_sqe_cancel(stream_handle);
				if(err)
				{
					LOG_WRN("failed to stop IMU FIFO stream: %d", err);
				}
				else
				{
					cancel_pending = true;
				}
			}
		}

		if(pending_stream_restart() != 0U ||
		   atomic_get(&imu_motion_trigger_enabled) == 0 ||
		   cancel_pending)
		{
			retry_ms = IMU_STREAM_RETRY_INITIAL_MS;
			continue;
		}

		k_msleep(retry_ms);
		retry_ms = MIN(retry_ms * 2U, IMU_STREAM_RETRY_MAX_MS);
	}
}

/*
 * Direct gyro/accel path. The FIFO only carries the SFLP words (quaternion and
 * drift estimate, 30 Hz); gyro and accel run at 2 kHz in the sensor and are read
 * straight from the output registers, both in one 12-byte I2C transaction.
 *
 * On USB the read is started by the host fetching the previous state report
 * (IN transfer done): the next report is then ready about half a frame before
 * the host's next poll, whatever point of the frame the host polls at, so each
 * poll gets a new sample (at most ~1 ms old). Without USB polls (Puck,
 * Bluetooth, nothing fetched for 1.5 ms) a 1 ms timer starts the reads.
 */
#define IMU_REG_CTRL6 0x15U /* FS_G [3:0], LPF1_G_BW [6:4] */
#define IMU_REG_CTRL7 0x16U /* LPF1_G_EN bit 0 */
#define IMU_REG_CTRL8 0x17U /* FS_XL [1:0] */
#define IMU_REG_OUTX_L_G 0x22U
#define IMU_PACE_TIMEOUT_MS 3U
#define IMU_USB_GAP_TICKS ((uint32_t)k_us_to_ticks_ceil32(1500))

static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(DT_ALIAS(accel0));
/* Zephyr LSM6DSV16X driver: sets the gyro ODR and remembers it across stream restarts */
int lsm6dsv16x_gyro_set_odr_raw(const struct device *dev, uint8_t odr);
K_SEM_DEFINE(imu_tick_sem, 0, 1);
static atomic_t imu_last_usb_ticks;
static atomic_t imu_last_usb_ms;
static atomic_t imu_fast_reads;
static atomic_t imu_fast_errors;
static atomic_t imu_last_read_ms;
static struct k_spinlock imu_stats_lock;
/* a report read is due (USB frame / timer) and/or the mid-frame gyro read (averaging) */
static atomic_t imu_tick_pending;
static atomic_t imu_mid_pending;

static void imu_tick(void)
{
	atomic_set(&imu_tick_pending, 1);
	k_sem_give(&imu_tick_sem);
}

static void imu_mid_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	atomic_set(&imu_mid_pending, 1);
	k_sem_give(&imu_tick_sem);
}
K_TIMER_DEFINE(imu_mid_timer, imu_mid_timer_fn, NULL);
static uint32_t imu_read_us_sum, imu_read_n, imu_read_us_max;

/* what starts a read on USB (Live setting imu_pace): 0 = USB frame start (SOF),
 * 1 = host fetched the previous report, 2 = 1 ms timer only */
static int16_t imu_pace_mode(void)
{
	return sc26re_cfg_get(SC26RE_CFG_IMU_PACE);
}

bool imu_paces_usb(void)
{
	uint32_t now = k_uptime_get_32();

	return now - (uint32_t)atomic_get(&imu_last_read_ms) <= IMU_PACE_TIMEOUT_MS &&
	       now - (uint32_t)atomic_get(&imu_last_usb_ms) <= IMU_PACE_TIMEOUT_MS;
}

static void imu_usb_event(int16_t mode)
{
	if(imu_pace_mode() != mode)
	{
		return;
	}
	atomic_set(&imu_last_usb_ticks, (atomic_val_t)(uint32_t)k_uptime_ticks());
	atomic_set(&imu_last_usb_ms, (atomic_val_t)k_uptime_get_32());
	imu_tick();
}

void imu_usb_sof(void)
{
	imu_usb_event(0);
}

void imu_usb_in_done(void)
{
	imu_usb_event(1);
}

void imu_fast_stats(uint32_t out[5])
{
	k_spinlock_key_t key = k_spin_lock(&imu_stats_lock);

	out[0] = (uint32_t)atomic_get(&imu_fast_reads);
	out[1] = (uint32_t)atomic_get(&imu_fast_errors);
	out[2] = imu_read_n ? imu_read_us_sum / imu_read_n : 0U;
	out[3] = imu_read_us_max;
	out[4] = (uint32_t)k_uptime_get_32();
	imu_read_us_sum = imu_read_n = imu_read_us_max = 0U;
	k_spin_unlock(&imu_stats_lock, key);
}

static void imu_tick_timer_fn(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	if(imu_pace_mode() == 2)
	{
		/* timer pacing: USB reports still wait for each new sample */
		atomic_set(&imu_last_usb_ms, (atomic_val_t)k_uptime_get_32());
		imu_tick();
		return;
	}
	/* only when the host is not polling (Puck / Bluetooth / idle USB) */
	if((uint32_t)k_uptime_ticks() - (uint32_t)atomic_get(&imu_last_usb_ticks) > IMU_USB_GAP_TICKS)
	{
		imu_tick();
	}
}
K_TIMER_DEFINE(imu_tick_timer, imu_tick_timer_fn, NULL);

/* sensor hardware low-pass on the gyro: 0 = off, 1..8 = LPF1 bandwidth 0..7 (widest..narrowest) */
static int imu_apply_gyro_hw_lpf(int16_t level)
{
	int err;

	if(level > 0)
	{
		err = i2c_reg_update_byte_dt(&imu_i2c, IMU_REG_CTRL6, 0x70U, (uint8_t)(((level - 1) & 7) << 4));
		if(err == 0)
		{
			err = i2c_reg_update_byte_dt(&imu_i2c, IMU_REG_CTRL7, 0x01U, 0x01U);
		}
	}
	else
	{
		err = i2c_reg_update_byte_dt(&imu_i2c, IMU_REG_CTRL7, 0x01U, 0x00U);
	}
	return err;
}

/* gyro (rad/s, sensor frame) -> pitch, yaw, roll in deg/s on the report axes, bias removed */
static void gyro_report_axes_dps(const float gyro[3], float out[3])
{
	float g[3];
	float o[3];

	for(size_t i = 0; i < 3; ++i)
	{
		float b = use_gyro_bias ? (latest_sflp_bias_valid ? latest_sflp_bias[i] : gyro_bias[i]) : 0.0f;

		g[i] = (gyro[i] - b) * (180.0f / 3.14159265f);
	}
	for(size_t row = 0; row < 3; ++row)
	{
		o[row] = 0.0f;
		for(size_t col = 0; col < 3; ++col)
		{
			o[row] += (float)mounting_matrix[row * 3 + col] * g[col];
		}
	}
	out[0] = o[0];  /* pitch */
	out[1] = o[2];  /* yaw */
	out[2] = -o[1]; /* roll */
}

static bool imu_send_mouse(void)
{
	int8_t dx, dy;
	uint8_t m[5] = { 0 };

	if(!gyro_mouse_take(&dx, &dy))
	{
		return false;
	}
	m[1] = (uint8_t)dx;
	m[2] = (uint8_t)dy;
	if(transport_send_input_report(0x40, m, sizeof(m)) != 0)
	{
		/* link busy: the movement stays for the next report */
		gyro_mouse_give_back(dx, dy);
		return false;
	}
	return true;
}

static void imu_fast_thread_main(void *arg1, void *arg2, void *arg3)
{
	/* LSM6DSV16X sensitivities: gyro 4.375 mdps/LSB at +-125 dps, doubling per step;
	 * accel 0.061 mg/LSB at +-2 g, doubling per step */
	static const uint8_t gyro_fs_shift[16] = { 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0 };
	float gyro_rad_per_lsb = 0.0f;
	float accel_ms2_per_lsb = 0.0f;
	int16_t applied_lpf = INT16_MIN;
	float gm_dps[3];
	uint32_t gm_wl_last = 0;
	uint32_t gm_last_us = 0;
	bool gm_have_state;
	int16_t mid_gyro_raw[3] = { 0 };
	bool mid_gyro_valid = false;
	bool avg2 = false;
	uint8_t reg;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	k_timer_start(&imu_tick_timer, K_USEC(1000), K_USEC(1000));
	for(;;)
	{
		uint8_t raw[12];
		struct controller_report report;
		float gyro[3];
		float accel[3];

		k_sem_take(&imu_tick_sem, K_FOREVER);
		if(atomic_get(&imu_motion_trigger_enabled) == 0 || atomic_get(&imu_stream_active) == 0)
		{
			/* the stream thread configures the sensor; wait for it */
			atomic_clear(&imu_mid_pending);
			atomic_clear(&imu_tick_pending);
			continue;
		}
		if(atomic_cas(&imu_mid_pending, 1, 0) && gyro_rad_per_lsb != 0.0f)
		{
			/* averaging: the gyro sample half a frame before the next report read */
			uint8_t g[6];

			if(i2c_burst_read_dt(&imu_i2c, IMU_REG_OUTX_L_G, g, sizeof(g)) == 0)
			{
				for(size_t i = 0; i < 3; ++i)
				{
					mid_gyro_raw[i] = (int16_t)sys_get_le16(&g[2 * i]);
				}
				mid_gyro_valid = true;
			}
		}
		if(!atomic_cas(&imu_tick_pending, 1, 0))
		{
			continue;
		}
		{
			/* gyro ODR in the sensor (Live setting): 2000/4000/8000 Hz (HAODR set 1).
			 * Still read once per frame; a higher rate gives the sensor's low-pass
			 * more samples to average internally, at no extra bus cost. */
			static int16_t applied_odr = 2000;
			int16_t odr = sc26re_cfg_get(SC26RE_CFG_GYRO_ODR);
			uint8_t raw = odr >= 8000 ? LSM6DSVXXX_DT_ODR_HA01_AT_8000Hz
			              : odr >= 4000 ? LSM6DSVXXX_DT_ODR_HA01_AT_4000Hz
			                            : LSM6DSVXXX_DT_ODR_HA01_AT_2000Hz;

			if(odr != applied_odr)
			{
				int err;

				k_mutex_lock(&imu_io_lock, K_FOREVER);
				err = lsm6dsv16x_gyro_set_odr_raw(imu_dev, raw);
				k_mutex_unlock(&imu_io_lock);
				if(err == 0)
				{
					applied_odr = odr;
					LOG_INF("gyro ODR %d Hz", odr);
				}
			}
		}
		{
			/* gyro full scale (Live setting): 250/500/1000/2000 dps -> FS_G code 1..4.
			 * Report units stay the same; a lower range gives finer steps and clips
			 * above it. Re-reading CTRL6 below picks up the new sensitivity. */
			static int16_t applied_fsr;
			int16_t fsr = sc26re_cfg_get(SC26RE_CFG_GYRO_FSR);
			uint8_t code = fsr <= 250 ? 1U : fsr <= 500 ? 2U : fsr <= 1000 ? 3U : 4U;

			if(fsr != applied_fsr &&
			   i2c_reg_update_byte_dt(&imu_i2c, IMU_REG_CTRL6, 0x0FU, code) == 0)
			{
				applied_fsr = fsr;
				gyro_rad_per_lsb = 0.0f;
			}
		}
		if(gyro_rad_per_lsb == 0.0f)
		{
			if(i2c_reg_read_byte_dt(&imu_i2c, IMU_REG_CTRL6, &reg) != 0)
			{
				atomic_inc(&imu_fast_errors);
				continue;
			}
			gyro_rad_per_lsb = 0.004375f * (float)(1U << gyro_fs_shift[reg & 0x0FU]) *
			                   (3.14159265f / 180.0f);
			if(i2c_reg_read_byte_dt(&imu_i2c, IMU_REG_CTRL8, &reg) != 0)
			{
				gyro_rad_per_lsb = 0.0f;
				atomic_inc(&imu_fast_errors);
				continue;
			}
			accel_ms2_per_lsb = 0.000061f * (float)(1U << (reg & 0x03U)) * 9.80665f;
			LOG_INF("direct IMU path: %d mdps/LSB, %d ug/LSB",
			        (int)(gyro_rad_per_lsb * 180.0f / 3.14159265f * 1000000.0f) / 1000,
			        (int)(accel_ms2_per_lsb / 9.80665f * 1000000.0f));
		}
#if DT_NODE_HAS_COMPAT(DT_BUS(DT_ALIAS(accel0)), nordic_nrf_twim)
		{
			/* IMU I2C bus speed (Live setting, 400 = standard; above is undocumented
			 * on nRF52833 but 800 ran in testing). The FREQUENCY word is a fraction
			 * of 16 MHz; only touched while this thread owns the latest transfer. */
			static int16_t applied_khz = 400;
			int16_t khz = sc26re_cfg_get(SC26RE_CFG_IMU_I2C_KHZ);

			if(khz != applied_khz)
			{
				NRF_TWIM_Type *twim = (NRF_TWIM_Type *)DT_REG_ADDR(DT_BUS(DT_ALIAS(accel0)));

				twim->FREQUENCY = khz <= 400 ? 0x06400000UL
				                             : (uint32_t)(((uint64_t)khz << 32) / 16000U);
				applied_khz = khz;
				LOG_INF("IMU I2C bus at %d kHz", khz);
			}
		}
#endif
		{
			int16_t lpf = sc26re_cfg_get(SC26RE_CFG_GYRO_HW_LPF);

			if(lpf != applied_lpf && imu_apply_gyro_hw_lpf(lpf) == 0)
			{
				applied_lpf = lpf;
			}
		}
		/* gyro X/Y/Z then accel X/Y/Z, little endian; BDU keeps each pair coherent */
		uint32_t c0 = k_cycle_get_32();

		if(i2c_burst_read_dt(&imu_i2c, IMU_REG_OUTX_L_G, raw, sizeof(raw)) != 0)
		{
			atomic_inc(&imu_fast_errors);
			continue;
		}
		{
			uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
			k_spinlock_key_t key = k_spin_lock(&imu_stats_lock);

			imu_read_us_sum += us;
			imu_read_n++;
			imu_read_us_max = MAX(imu_read_us_max, us);
			k_spin_unlock(&imu_stats_lock, key);
		}
		atomic_inc(&imu_fast_reads);
		atomic_set(&imu_last_read_ms, (atomic_val_t)k_uptime_get_32());
		avg2 = sc26re_cfg_get(SC26RE_CFG_GYRO_AVG) >= 2;
		for(size_t i = 0; i < 3; ++i)
		{
			int32_t g = (int16_t)sys_get_le16(&raw[2 * i]);

			/* 2-sample average (Live setting gyro_avg = 2): this read + the one
			 * half a frame earlier; halves the bandwidth like a light low-pass */
			gyro[i] = (avg2 && mid_gyro_valid ? (float)(g + mid_gyro_raw[i]) * 0.5f : (float)g) *
			          gyro_rad_per_lsb;
			accel[i] = (float)(int16_t)sys_get_le16(&raw[6 + 2 * i]) * accel_ms2_per_lsb;
		}

		mid_gyro_valid = false;
		if(avg2)
		{
			/* half a frame after this read started, clear of the next one */
			uint32_t done_us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

			k_timer_start(&imu_mid_timer, K_USEC(done_us < 450U ? 500U - done_us : 50U), K_NO_WAIT);
		}
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		load_cached_imu_report(&report);
		process_accel(accel, &report);
		process_gyro(gyro, &report);
		report.imu_timestamp_us = (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
		store_cached_imu_report(&report);
		gyro_report_axes_dps(gyro, gm_dps);
		k_mutex_unlock(&imu_io_lock);
		{
			/* gyro as mouse: feed every sample (also handles its button chords) */
			struct controller_report st;
			uint32_t now_us = report.imu_timestamp_us;

			gm_have_state = hardware_load_state(&st);
			gyro_mouse_feed(gm_dps, gm_have_state ? st.buttons : 0U,
			                gm_last_us ? (float)(now_us - gm_last_us) * 1e-6f : 0.001f);
			gm_last_us = now_us;
		}
		if(transport_usb_state_reports_allowed() && imu_paces_usb() &&
		   sc26re_cfg_get(SC26RE_CFG_REPORT_RATE) >= 1000)
		{
			/*
			 * Send the new sample right here with the report loop's latest
			 * buttons/sticks/pads: going through the report loop added its
			 * scan time and sometimes missed the host's next poll.
			 *
			 * Gyro mouse: the movement goes out as a mouse report; the USB
			 * transport merges it and shares the frames with the pad report.
			 */
			struct controller_report state;

			if(gyro_mouse_has_counts())
			{
				(void)imu_send_mouse();
			}
			if(hardware_load_state(&state))
			{
				load_cached_imu_report(&state);
				hide_motion_if_mouse(&state);
				(void)transport_usb_send(&state);
			}
			else
			{
				hardware_signal_input_changed();
			}
		}
		else if(transport_usb_configured() || transport_esb_connected() || transport_ble_connected())
		{
			/* Puck / Bluetooth: mouse on every sample (the link merges waiting mouse
			 * reports), the report loop sends the pad */
			/* steady cadence the Puck can pass on: a mouse report every
			 * 1000/gm_rate_wl ms, movement in between is merged */
			uint32_t now = (uint32_t)k_uptime_ticks();
			/* a bit under the period, so the ~1 ms reads don't slip a cycle */
			uint32_t period = k_us_to_ticks_floor32(
			    1000000U / (uint32_t)MAX(sc26re_cfg_get(SC26RE_CFG_GM_RATE_WL), 125) - 300U);

			if(now - gm_wl_last >= period && gyro_mouse_has_counts() && imu_send_mouse())
			{
				gm_wl_last = now;
			}
			hardware_signal_input_changed();
		}
	}
}
#else
void imu_usb_in_done(void)
{
}

void imu_usb_sof(void)
{
}

void imu_fast_stats(uint32_t out[5])
{
	memset(out, 0, 5 * sizeof(uint32_t));
}

bool imu_paces_usb(void)
{
	return false;
}
#endif

int imu_read_report(struct controller_report *report)
{
	bool enabled;

	/*
	 * No imu_io_lock here: the IMU thread holds it while decoding each FIFO batch,
	 * which used to stall the report loop. imu_mode is a naturally atomic halfword
	 * and the cached report has its own spinlock.
	 */
	enabled = *(volatile uint16_t *)&imu_mode != SETTING_GYRO_MODE_OFF;
	if(!enabled)
	{
		return 0;
	}

	load_cached_imu_report(report);
	hide_motion_if_mouse(report);
	return 0;
}

/*
 * Gyro calibration by measurement (Steam's 0xB5 and the report meter). OFW
 * copies the sensor-fusion bias estimate, which is only right once the
 * controller has been still long enough for it to converge; pressed too early
 * it saved a wrong bias and the gyro drifted. Here the raw gyro is averaged
 * for GYRO_CAL_SAMPLES samples; if the controller moved, nothing is changed.
 */
#define GYRO_CAL_SAMPLES 1500
/*
 * "Moved" test on the standard deviation, not min..max: single outlier samples
 * (FIFO/stream glitches) made a controller lying on a table fail the old
 * min..max check. Hand tremor is several deg/s; a still controller ~0.1.
 */
#define GYRO_CAL_MAX_STDDEV 0.015f /* rad/s (~0.86 deg/s) */
static atomic_t gyro_cal_active;
/* 0 = idle, 1 = measuring, 2 = saved, 3 = moved (old bias kept), 4 = save failed */
static atomic_t gyro_cal_state;

uint8_t imu_gyro_cal_state(void)
{
	return (uint8_t)atomic_get(&gyro_cal_state);
}
static uint32_t gyro_cal_n;
static double gyro_cal_sum[3];
static double gyro_cal_sumsq[3];
/* last result for the report meter: per-axis std dev and spread in mdeg/s */
static int16_t gyro_cal_std_mdps[3];
static int16_t gyro_cal_spread_mdps[3];

void imu_gyro_cal_result(int16_t std_mdps[3], int16_t spread_mdps[3])
{
	memcpy(std_mdps, gyro_cal_std_mdps, sizeof(gyro_cal_std_mdps));
	memcpy(spread_mdps, gyro_cal_spread_mdps, sizeof(gyro_cal_spread_mdps));
}
static float gyro_cal_min[3];
static float gyro_cal_max[3];
static void gyro_cal_work_handler(struct k_work *work);
static K_WORK_DEFINE(gyro_cal_work, gyro_cal_work_handler);

static void gyro_cal_sample(const float gyro[3])
{
	if(!atomic_get(&gyro_cal_active))
	{
		return;
	}
	for(size_t i = 0; i < 3; ++i)
	{
		gyro_cal_sum[i] += (double)gyro[i];
		gyro_cal_sumsq[i] += (double)gyro[i] * (double)gyro[i];
		if(gyro_cal_n == 0)
		{
			gyro_cal_min[i] = gyro[i];
			gyro_cal_max[i] = gyro[i];
		}
		gyro_cal_min[i] = MIN(gyro_cal_min[i], gyro[i]);
		gyro_cal_max[i] = MAX(gyro_cal_max[i], gyro[i]);
	}
	if(++gyro_cal_n >= GYRO_CAL_SAMPLES)
	{
		atomic_set(&gyro_cal_active, 0);
		k_work_submit(&gyro_cal_work);
	}
}

static int imu_save_gyro_bias(const float bias_in[3], bool persist);

/*
 * Automatic calibration (controller put down): the same measurement, but the
 * result is only applied when it differs from the current bias, and written to
 * flash at most every 5 minutes (or on a big change) to spare the flash.
 */
#define GYRO_AUTO_MIN_CHANGE 0.0005f   /* rad/s (~0.03 deg/s): smaller is ignored */
#define GYRO_AUTO_BIG_CHANGE 0.005f    /* rad/s (~0.3 deg/s): saved right away */
#define GYRO_AUTO_SAVE_INTERVAL_MS (5 * 60 * 1000)
static bool gyro_cal_auto;
static int64_t gyro_auto_last_save_ms = -GYRO_AUTO_SAVE_INTERVAL_MS;

static void gyro_cal_work_handler(struct k_work *work)
{
	float bias[3];
	bool moved = false;

	ARG_UNUSED(work);
	for(size_t i = 0; i < 3; ++i)
	{
		double mean = gyro_cal_sum[i] / (double)gyro_cal_n;
		double var = gyro_cal_sumsq[i] / (double)gyro_cal_n - mean * mean;
		float sd = var > 0.0 ? sqrtf((float)var) : 0.0f;
		const float mdps_per_rad = 57295.78f;

		bias[i] = (float)mean;
		gyro_cal_std_mdps[i] = (int16_t)CLAMP((int32_t)(sd * mdps_per_rad), 0, INT16_MAX);
		gyro_cal_spread_mdps[i] = (int16_t)CLAMP(
		    (int32_t)((gyro_cal_max[i] - gyro_cal_min[i]) * mdps_per_rad), 0, INT16_MAX);
		if(sd > GYRO_CAL_MAX_STDDEV)
		{
			moved = true;
		}
	}
	LOG_INF("gyro calibration: std %d %d %d mdps, spread %d %d %d mdps", gyro_cal_std_mdps[0],
	        gyro_cal_std_mdps[1], gyro_cal_std_mdps[2], gyro_cal_spread_mdps[0],
	        gyro_cal_spread_mdps[1], gyro_cal_spread_mdps[2]);
	if(moved)
	{
		atomic_set(&gyro_cal_state, 3);
		LOG_WRN("gyro calibration: controller moved, kept old bias");
		return;
	}
	LOG_INF("gyro calibration measured bias %d %d %d urad/s", (int)(bias[0] * 1e6f),
	        (int)(bias[1] * 1e6f), (int)(bias[2] * 1e6f));
	if(gyro_cal_auto)
	{
		float change = 0.0f;
		bool persist;

		for(size_t i = 0; i < 3; ++i)
		{
			change = MAX(change, fabsf(bias[i] - gyro_bias[i]));
		}
		if(change < GYRO_AUTO_MIN_CHANGE)
		{
			atomic_set(&gyro_cal_state, 2);
			return;
		}
		persist = change >= GYRO_AUTO_BIG_CHANGE ||
		          k_uptime_get() - gyro_auto_last_save_ms >= GYRO_AUTO_SAVE_INTERVAL_MS;
		if(persist)
		{
			gyro_auto_last_save_ms = k_uptime_get();
		}
		LOG_INF("auto gyro calibration: change %d urad/s%s", (int)(change * 1e6f),
		        persist ? ", saved" : " (RAM only)");
		atomic_set(&gyro_cal_state, imu_save_gyro_bias(bias, persist) == 0 ? 2 : 4);
		return;
	}
	atomic_set(&gyro_cal_state, imu_save_gyro_bias(bias, true) == 0 ? 2 : 4);
}

int imu_calibrate_gyro_auto(void)
{
	int err = imu_calibrate_gyro();

	gyro_cal_auto = true;
	return err;
}

bool imu_streaming(void)
{
	return atomic_get(&imu_motion_trigger_enabled) != 0;
}

int imu_calibrate_gyro(void)
{
	gyro_cal_auto = false;
	memset(gyro_cal_sum, 0, sizeof(gyro_cal_sum));
	memset(gyro_cal_sumsq, 0, sizeof(gyro_cal_sumsq));
	gyro_cal_n = 0;
	atomic_set(&gyro_cal_state, 1);
	atomic_set(&gyro_cal_active, 1);
	LOG_INF("gyro calibration: measuring, keep the controller still");
	return 0;
}

static int imu_save_gyro_bias(const float bias_in[3], bool persist)
{
	float saved_bias[ARRAY_SIZE(gyro_bias)];
	int err;

	k_mutex_lock(&imu_settings_mutex, K_FOREVER);
	k_mutex_lock(&imu_io_lock, K_FOREVER);
	memcpy(gyro_bias, bias_in, sizeof(gyro_bias));
	memcpy(gyro_bias_candidate, bias_in, sizeof(gyro_bias_candidate));
	/* use it right away, until the fusion engine reports its own estimate again */
	memcpy(latest_sflp_bias, bias_in, sizeof(latest_sflp_bias));
	memcpy(staged_gyro_bias, gyro_bias, sizeof(staged_gyro_bias));
	memcpy(saved_bias, gyro_bias, sizeof(saved_bias));
	gyro_bias_dirty = false;
	last_sflp_bias_update_us = 0;
	k_mutex_unlock(&imu_io_lock);

	err = program_gyro_bias();
	if(err)
	{
		LOG_WRN("failed to program gyro bias: %d", err);
		k_mutex_unlock(&imu_settings_mutex);
		return err;
	}

	if(!IS_ENABLED(CONFIG_SETTINGS) || !persist)
	{
		k_mutex_unlock(&imu_settings_mutex);
		return 0;
	}

	err = settings_save_one(IMU_GYRO_BIAS_PATH, saved_bias, sizeof(saved_bias));
	if(err)
	{
		LOG_WRN("failed to save gyro bias: %d", err);
		k_mutex_unlock(&imu_settings_mutex);
		return err;
	}
	k_mutex_unlock(&imu_settings_mutex);
	LOG_INF("saved gyro biases");
	return 0;
}

/*
 * Report meter "Reset gyro calibration": forget sc26re's saved drift and go
 * back to the factory value from Valve's storage (0 if there is none). Applied
 * right away; the factory value is written back as the saved one.
 */
int imu_reset_gyro_calibration(void)
{
	float bias[3] = { 0 };
	int err;

	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		return -ENOTSUP;
	}
	(void)settings_delete(IMU_GYRO_BIAS_PATH);
	(void)calibration_import_imu_from_valve_storage();
	if(settings_load_one(IMU_GYRO_BIAS_PATH, bias, sizeof(bias)) != (ssize_t)sizeof(bias))
	{
		memset(bias, 0, sizeof(bias));
	}
	err = imu_save_gyro_bias(bias, false);
	atomic_set(&gyro_cal_state, 0);
	LOG_INF("gyro calibration reset to factory: %d %d %d urad/s (err %d)", (int)(bias[0] * 1e6f),
	        (int)(bias[1] * 1e6f), (int)(bias[2] * 1e6f), err);
	return err;
}

bool imu_settings_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
	if(strcmp(path, IMU_USE_BIAS_PATH) == 0 || strcmp(path, IMU_GYRO_THRESHOLD_PATH) == 0)
	{
		uint8_t id = strcmp(path, IMU_USE_BIAS_PATH) == 0 ? IBEX_SETTING_IMU_USE_BIAS
		                                                   : IBEX_SETTING_IMU_GYRO_THRESHOLD;
		int16_t value;

		if(capacity < 1 || !ibex_setting_get(id, &value))
		{
			return false;
		}
		buf[0] = (uint8_t)value;
		*len = 1;
		return true;
	}
	if(strcmp(path, IMU_MOUNTING_MATRIX_PATH) == 0)
	{
		if(capacity < sizeof(mounting_matrix))
		{
			return false;
		}
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		memcpy(buf, mounting_matrix, sizeof(mounting_matrix));
		k_mutex_unlock(&imu_io_lock);
		*len = sizeof(mounting_matrix);
		return true;
	}
	if(strcmp(path, IMU_GYRO_DZ_THRESHOLD_PATH) == 0)
	{
		if(capacity < sizeof(int32_t))
		{
			return false;
		}
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		sys_put_le32((uint32_t)gyro_dz_threshold, buf);
		k_mutex_unlock(&imu_io_lock);
		*len = sizeof(int32_t);
		return true;
	}
	if(strcmp(path, IMU_GYRO_BIAS_PATH) == 0)
	{
		if(capacity < sizeof(gyro_bias))
		{
			return false;
		}
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		memcpy(buf, gyro_bias, sizeof(gyro_bias));
		k_mutex_unlock(&imu_io_lock);
		*len = sizeof(gyro_bias);
		return true;
	}

	return false;
}

int imu_settings_stage(const char *path, const uint8_t *value, size_t len)
{
	float received_bias[3];
	int err;

	if(strcmp(path, IMU_USE_BIAS_PATH) == 0 || strcmp(path, IMU_GYRO_THRESHOLD_PATH) == 0)
	{
		uint8_t id = strcmp(path, IMU_USE_BIAS_PATH) == 0 ? IBEX_SETTING_IMU_USE_BIAS
		                                                   : IBEX_SETTING_IMU_GYRO_THRESHOLD;
		int16_t normalized;

		if(len != 1)
		{
			return -EINVAL;
		}
		k_mutex_lock(&imu_settings_mutex, K_FOREVER);
		err = ibex_setting_set(id, value[0]);
		if(!err)
		{
			(void)ibex_setting_get(id, &normalized);
			if(id == IBEX_SETTING_IMU_USE_BIAS)
			{
				staged_use_gyro_bias = (uint8_t)normalized;
				use_gyro_bias_dirty = true;
			}
			else
			{
				staged_gyro_threshold = (uint8_t)normalized;
				gyro_threshold_dirty = true;
			}
		}
		k_mutex_unlock(&imu_settings_mutex);
		return err;
	}

	if(strcmp(path, IMU_MOUNTING_MATRIX_PATH) == 0)
	{
		if(len != sizeof(mounting_matrix))
		{
			return -EINVAL;
		}
		k_mutex_lock(&imu_settings_mutex, K_FOREVER);
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		memcpy(staged_mounting_matrix, value, sizeof(staged_mounting_matrix));
		memcpy(mounting_matrix, staged_mounting_matrix, sizeof(mounting_matrix));
		mounting_matrix_dirty = true;
		k_mutex_unlock(&imu_io_lock);
		k_mutex_unlock(&imu_settings_mutex);
		return 0;
	}
	if(strcmp(path, IMU_GYRO_DZ_THRESHOLD_PATH) == 0)
	{
		if(len != sizeof(int32_t))
		{
			return -EINVAL;
		}
		k_mutex_lock(&imu_settings_mutex, K_FOREVER);
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		staged_gyro_dz_threshold = (int32_t)sys_get_le32(value);
		gyro_dz_threshold = staged_gyro_dz_threshold;
		gyro_dz_threshold_dirty = true;
		k_mutex_unlock(&imu_io_lock);
		k_mutex_unlock(&imu_settings_mutex);
		return 0;
	}
	if(strcmp(path, IMU_GYRO_BIAS_PATH) == 0)
	{
		if(len != sizeof(gyro_bias))
		{
			return -EINVAL;
		}
		memcpy(received_bias, value, sizeof(received_bias));
		if(!imu_gyro_bias_valid(received_bias))
		{
			return -ERANGE;
		}
		k_mutex_lock(&imu_settings_mutex, K_FOREVER);
		k_mutex_lock(&imu_io_lock, K_FOREVER);
		memcpy(staged_gyro_bias, received_bias, sizeof(staged_gyro_bias));
		memcpy(gyro_bias, staged_gyro_bias, sizeof(gyro_bias));
		memcpy(gyro_bias_candidate, gyro_bias, sizeof(gyro_bias_candidate));
		gyro_bias_dirty = true;
		last_sflp_bias_update_us = 0;
		k_mutex_unlock(&imu_io_lock);
		err = program_gyro_bias();
		k_mutex_unlock(&imu_settings_mutex);
		if(err)
		{
			return err;
		}
		return 0;
	}

	return -ENOENT;
}

static int commit_setting_if_dirty(const char *path, const void *value, size_t len, bool *dirty)
{
	int err;

	k_mutex_lock(&imu_settings_mutex, K_FOREVER);
	if(!*dirty)
	{
		k_mutex_unlock(&imu_settings_mutex);
		return 0;
	}
	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		err = 0;
	}
	else
	{
		err = settings_save_one(path, value, len);
	}
	if(!err)
	{
		*dirty = false;
	}
	k_mutex_unlock(&imu_settings_mutex);
	return err;
}

int imu_settings_commit(const char *path)
{
	if(strcmp(path, IMU_USE_BIAS_PATH) == 0)
	{
		return commit_setting_if_dirty(path, &staged_use_gyro_bias,
		                               sizeof(staged_use_gyro_bias), &use_gyro_bias_dirty);
	}
	if(strcmp(path, IMU_GYRO_THRESHOLD_PATH) == 0)
	{
		return commit_setting_if_dirty(path, &staged_gyro_threshold,
		                               sizeof(staged_gyro_threshold), &gyro_threshold_dirty);
	}
	if(strcmp(path, IMU_MOUNTING_MATRIX_PATH) == 0)
	{
		return commit_setting_if_dirty(path, staged_mounting_matrix, sizeof(staged_mounting_matrix),
		                               &mounting_matrix_dirty);
	}
	if(strcmp(path, IMU_GYRO_DZ_THRESHOLD_PATH) == 0)
	{
		return commit_setting_if_dirty(path, &staged_gyro_dz_threshold,
		                               sizeof(staged_gyro_dz_threshold), &gyro_dz_threshold_dirty);
	}
	if(strcmp(path, IMU_GYRO_BIAS_PATH) == 0)
	{
		return commit_setting_if_dirty(path, staged_gyro_bias, sizeof(staged_gyro_bias),
		                               &gyro_bias_dirty);
	}

	return -ENOENT;
}
