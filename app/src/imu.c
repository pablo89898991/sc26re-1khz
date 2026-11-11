/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/lsm6dsv16x.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "controller.h"
#if CONFIG_IBEX_IMU_DEBUG_IN_QUAT
#include <zephyr/drivers/sensor_clock.h>
#endif
#if CONFIG_IBEX_IMU_I2C_FREQ_KHZ != 400
#include <hal/nrf_twim.h>
#endif
#include "ibex_settings_registry.h"
#include "imu.h"
#include "imu_bias.h"
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

static void set_motion_trigger_enabled(bool enabled)
{
#if IMU_HAS_DEVICE
	atomic_val_t previous = atomic_set(&imu_motion_trigger_enabled, enabled ? 1 : 0);

	if(enabled && previous == 0)
	{
		k_sem_give(&imu_stream_start_sem);
	}
#else
	ARG_UNUSED(enabled);
#endif
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

static int build_imu_report_from_stream(const uint8_t *buf)
{
	const struct sensor_decoder_api *decoder;
	struct controller_report report = { 0 };
	float accel[3];
	float gyro[3];
	float sflp_bias[3];
	float quaternion[4];
	float bias[3];
	uint32_t timestamp_us = 0;
	int16_t oriented[3];
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
		oriented[0] = accel_axis_to_report(accel[0]);
		oriented[1] = accel_axis_to_report(accel[1]);
		oriented[2] = accel_axis_to_report(accel[2]);
		apply_orientation_i16(oriented);
		report.accel_x = oriented[0];
		report.accel_y = oriented[1];
		report.accel_z = oriented[2];
		updated = true;
	}

	err = decode_latest_three_axis_ts(decoder, buf, SENSOR_CHAN_GYRO_XYZ, gyro, &gyro_timestamp_ns);
	if(err == 0)
	{
		memcpy(bias, latest_sflp_bias_valid ? latest_sflp_bias : gyro_bias, sizeof(bias));
		if(!use_gyro_bias)
		{
			memset(bias, 0, sizeof(bias));
		}
		oriented[0] = gyro_axis_to_report(gyro[0] - bias[0]);
		oriented[1] = gyro_axis_to_report(gyro[1] - bias[1]);
		oriented[2] = gyro_axis_to_report(gyro[2] - bias[2]);
		apply_orientation_i16(oriented);
		for(size_t i = 0; i < 3; ++i)
		{
			if(-gyro_dz_threshold < oriented[i] && gyro_dz_threshold > oriented[i])
			{
				oriented[i] = 0;
			}
		}
		report.gyro_x = oriented[0];
		report.gyro_y = oriented[1];
		report.gyro_z = oriented[2];
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
		if(gyro_updated && transport_usb_configured())
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
	return 0;
}

int imu_calibrate_gyro(void)
{
	float saved_bias[ARRAY_SIZE(gyro_bias)];
	int err;

	k_mutex_lock(&imu_settings_mutex, K_FOREVER);
	k_mutex_lock(&imu_io_lock, K_FOREVER);
	memcpy(gyro_bias, gyro_bias_candidate, sizeof(gyro_bias));
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

	if(!IS_ENABLED(CONFIG_SETTINGS))
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
