/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Experimental SInput HID protocol for the USB link.
 *
 * Report layout, HID descriptor and command set follow Hand Held Legend's
 * SINPUT-LIB-HID (MIT-0) and the SDL3 hidapi SInput driver. Axis/IMU sign
 * conventions are converted from the Triton (Valve) report so SDL produces the
 * same values it would for the stock Steam Controller driver.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "controller.h"
#include "haptics.h"
#include "sdl/controller_structs.h"
#include "sinput_usb.h"
#include "valve_identity.h"

#if CONFIG_IBEX_RGBW_LED
#include "rgbw_led.h"
#endif

LOG_MODULE_REGISTER(sinput_usb);

#define SINPUT_CMD_HAPTIC 0x01U
#define SINPUT_CMD_FEATURES 0x02U
#define SINPUT_CMD_PLAYERLED 0x03U
#define SINPUT_CMD_JOYSTICKRGB 0x04U

#define SINPUT_PROTOCOL_VERSION 0x0001U
#define SINPUT_POLLING_RATE_US 1000U
#define SINPUT_ACCEL_RANGE_G 2U       /* matches the Triton report scale */
#define SINPUT_GYRO_RANGE_DPS 2000U   /* matches the Triton report scale */
#define SINPUT_GAMEPAD_TYPE_XBOXONE 3U
#define SINPUT_FACE_STYLE_ABXY 1U

#define SINPUT_RUMBLE_REFRESH_MS 40
#define SINPUT_RUMBLE_TIMEOUT_MS 2500

/* Byte offsets inside the 64-byte input report (including report ID). */
#define SI_PLUG 1
#define SI_CHARGE 2
#define SI_BUTTONS 3
#define SI_LX 7
#define SI_LY 9
#define SI_RX 11
#define SI_RY 13
#define SI_LT 15
#define SI_RT 17
#define SI_IMU_TS 19
#define SI_ACCEL_X 23
#define SI_ACCEL_Y 25
#define SI_ACCEL_Z 27
#define SI_GYRO_X 29
#define SI_GYRO_Y 31
#define SI_GYRO_Z 33
#define SI_T1_X 35
#define SI_T1_Y 37
#define SI_T1_P 39
#define SI_T2_X 41
#define SI_T2_Y 43
#define SI_T2_P 45

/* HID report descriptor from SINPUT-LIB-HID (MIT-0, Hand Held Legend). */
const uint8_t sinput_hid_report_map[] = {
	0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
	/* Input report 0x01 */
	0x85, 0x01,
	0x06, 0x00, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x20, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,
	0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x06, 0x81, 0x02,
	0x06, 0x00, 0xFF,
	0x09, 0x20, 0x15, 0x00, 0x26, 0xFF, 0xFF, 0x75, 0x20, 0x95, 0x01, 0x81, 0x02,
	0x09, 0x21, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x06, 0x81, 0x02,
	0x09, 0x22, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x1D, 0x81, 0x02,
	/* Input report 0x02 (command replies) */
	0x85, 0x02, 0x09, 0x23, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x02,
	/* Output report 0x03 (commands) */
	0x85, 0x03, 0x09, 0x24, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x2F, 0x91, 0x02,
	0xC0,
};
const size_t sinput_hid_report_map_size = sizeof(sinput_hid_report_map);
BUILD_ASSERT(sizeof(sinput_hid_report_map) == 139, "SInput descriptor must be 139 bytes");

static atomic_t battery_plug = ATOMIC_INIT(2);    /* 2 = charging (USB powered) */
static atomic_t battery_percent = ATOMIC_INIT(100);

static struct k_spinlock rumble_lock;
static uint16_t rumble_left;
static uint16_t rumble_right;
static int64_t rumble_last_cmd_ms;
static void rumble_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rumble_work, rumble_work_handler);

static inline bool btn(const struct controller_report *r, enum controller_button b)
{
	return (r->buttons & BIT((uint32_t)b)) != 0U;
}

static inline int16_t neg16(int16_t v)
{
	return v == INT16_MIN ? INT16_MAX : (int16_t)-v;
}

static inline int16_t trigger16(int16_t v)
{
	int32_t t = CLAMP((int32_t)v, 0, 32767);

	return (int16_t)CLAMP(t * 2 - 32768, INT16_MIN, INT16_MAX);
}

void sinput_note_battery(const struct controller_battery_report *report)
{
	uint8_t plug;

	if(report == NULL || !report->valid)
	{
		return;
	}
	switch(report->charge_state)
	{
		case CONTROLLER_CHARGE_STATE_CHARGING_DONE:
			plug = 3;
			break;
		case CONTROLLER_CHARGE_STATE_DISCHARGING:
			plug = 4;
			break;
		default:
			plug = 2;
			break;
	}
	if(report->charge_complete)
	{
		plug = 3;
	}
	atomic_set(&battery_plug, plug);
	atomic_set(&battery_percent, MIN(report->level_percent, 100));
}

size_t sinput_build_input_report(uint8_t out[SINPUT_REPORT_SIZE], const struct controller_report *r)
{
	uint8_t b1 = 0;
	uint8_t b2 = 0;
	uint8_t b3 = 0;
	uint16_t p;
	bool touch;

	memset(out, 0, SINPUT_REPORT_SIZE);
	out[0] = SINPUT_REPORT_ID_INPUT;
	out[SI_PLUG] = (uint8_t)atomic_get(&battery_plug);
	out[SI_CHARGE] = (uint8_t)atomic_get(&battery_percent);

	b1 |= btn(r, CONTROLLER_BUTTON_A) ? BIT(0) : 0;            /* south */
	b1 |= btn(r, CONTROLLER_BUTTON_B) ? BIT(1) : 0;            /* east */
	b1 |= btn(r, CONTROLLER_BUTTON_X) ? BIT(2) : 0;            /* west */
	b1 |= btn(r, CONTROLLER_BUTTON_Y) ? BIT(3) : 0;            /* north */
	b1 |= btn(r, CONTROLLER_BUTTON_DPAD_UP) ? BIT(4) : 0;
	b1 |= btn(r, CONTROLLER_BUTTON_DPAD_DOWN) ? BIT(5) : 0;
	b1 |= btn(r, CONTROLLER_BUTTON_DPAD_LEFT) ? BIT(6) : 0;
	b1 |= btn(r, CONTROLLER_BUTTON_DPAD_RIGHT) ? BIT(7) : 0;

	b2 |= btn(r, CONTROLLER_BUTTON_LEFT_STICK) ? BIT(0) : 0;
	b2 |= btn(r, CONTROLLER_BUTTON_RIGHT_STICK) ? BIT(1) : 0;
	b2 |= btn(r, CONTROLLER_BUTTON_LEFT_SHOULDER) ? BIT(2) : 0;
	b2 |= btn(r, CONTROLLER_BUTTON_RIGHT_SHOULDER) ? BIT(3) : 0;
	b2 |= btn(r, CONTROLLER_BUTTON_LEFT_TRIGGER_CLICK) ? BIT(4) : 0;
	b2 |= btn(r, CONTROLLER_BUTTON_RIGHT_TRIGGER_CLICK) ? BIT(5) : 0;
	b2 |= btn(r, CONTROLLER_BUTTON_LEFT_PADDLE1) ? BIT(6) : 0;   /* L4 */
	b2 |= btn(r, CONTROLLER_BUTTON_RIGHT_PADDLE1) ? BIT(7) : 0;  /* R4 */

	b3 |= btn(r, CONTROLLER_BUTTON_MENU) ? BIT(0) : 0;           /* start */
	b3 |= btn(r, CONTROLLER_BUTTON_VIEW) ? BIT(1) : 0;           /* back */
	b3 |= btn(r, CONTROLLER_BUTTON_STEAM) ? BIT(2) : 0;          /* guide */
	b3 |= btn(r, CONTROLLER_BUTTON_QAM) ? BIT(3) : 0;            /* share */
	b3 |= btn(r, CONTROLLER_BUTTON_LEFT_PADDLE2) ? BIT(4) : 0;   /* L5 */
	b3 |= btn(r, CONTROLLER_BUTTON_RIGHT_PADDLE2) ? BIT(5) : 0;  /* R5 */
	b3 |= btn(r, CONTROLLER_BUTTON_LEFT_TOUCHPAD_CLICK) ? BIT(6) : 0;
	b3 |= btn(r, CONTROLLER_BUTTON_RIGHT_TOUCHPAD_CLICK) ? BIT(7) : 0;

	out[SI_BUTTONS + 0] = b1;
	out[SI_BUTTONS + 1] = b2;
	out[SI_BUTTONS + 2] = b3;
	out[SI_BUTTONS + 3] = 0;

	/* SDL: Triton Y axes are up-positive and get negated; SInput is sent as-is. */
	sys_put_le16((uint16_t)r->stick_left_x, &out[SI_LX]);
	sys_put_le16((uint16_t)neg16(r->stick_left_y), &out[SI_LY]);
	sys_put_le16((uint16_t)r->stick_right_x, &out[SI_RX]);
	sys_put_le16((uint16_t)neg16(r->stick_right_y), &out[SI_RY]);
	sys_put_le16((uint16_t)trigger16(r->trigger_left), &out[SI_LT]);
	sys_put_le16((uint16_t)trigger16(r->trigger_right), &out[SI_RT]);

	sys_put_le32(r->imu_timestamp_us != 0 ? r->imu_timestamp_us : k_cyc_to_us_floor32(k_cycle_get_32()),
	             &out[SI_IMU_TS]);

	/*
	 * Triton -> SDL: (x, z, -y). SInput -> SDL: (-x, z, -y).
	 * So SInput x = -Triton x, y and z unchanged.
	 */
	sys_put_le16((uint16_t)neg16(r->accel_x), &out[SI_ACCEL_X]);
	sys_put_le16((uint16_t)r->accel_y, &out[SI_ACCEL_Y]);
	sys_put_le16((uint16_t)r->accel_z, &out[SI_ACCEL_Z]);
	sys_put_le16((uint16_t)neg16(r->gyro_x), &out[SI_GYRO_X]);
	sys_put_le16((uint16_t)r->gyro_y, &out[SI_GYRO_Y]);
	sys_put_le16((uint16_t)r->gyro_z, &out[SI_GYRO_Z]);

	/* Touchpads: SDL uses pressure > 0 as "finger down"; Y is up-positive on Triton. */
	touch = btn(r, CONTROLLER_BUTTON_LEFT_TOUCHPAD_TOUCH);
	p = touch ? MAX(MIN(r->touchpad_left_pressure, 32767U), 1U) : 0U;
	sys_put_le16((uint16_t)r->touchpad_left_x, &out[SI_T1_X]);
	sys_put_le16((uint16_t)neg16(r->touchpad_left_y), &out[SI_T1_Y]);
	sys_put_le16(p, &out[SI_T1_P]);

	touch = btn(r, CONTROLLER_BUTTON_RIGHT_TOUCHPAD_TOUCH);
	p = touch ? MAX(MIN(r->touchpad_right_pressure, 32767U), 1U) : 0U;
	sys_put_le16((uint16_t)r->touchpad_right_x, &out[SI_T2_X]);
	sys_put_le16((uint16_t)neg16(r->touchpad_right_y), &out[SI_T2_Y]);
	sys_put_le16(p, &out[SI_T2_P]);

	return SINPUT_REPORT_SIZE;
}

static void build_features(uint8_t out[SINPUT_REPORT_SIZE])
{
	const char *serial = valve_identity_serial(VALVE_IDENTITY_UNIT_SERIAL);
	size_t serial_len = serial ? strlen(serial) : 0;
	uint8_t ff1;
	uint8_t ff2;

	memset(out, 0, SINPUT_REPORT_SIZE);
	out[0] = SINPUT_REPORT_ID_INPUT_CMDDAT;
	out[1] = SINPUT_CMD_FEATURES;
	sys_put_le16(SINPUT_PROTOCOL_VERSION, &out[2]);

	/* rumble | accel | gyro | L stick | R stick | L analog trig | R analog trig */
	ff1 = BIT(0) | BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7);
	/* touchpad | joystick RGB */
	ff2 = BIT(0);
#if CONFIG_IBEX_RGBW_LED
	ff2 |= BIT(1);
#endif
	out[4] = ff1;
	out[5] = ff2;
	out[6] = SINPUT_GAMEPAD_TYPE_XBOXONE;
	out[7] = (uint8_t)(SINPUT_FACE_STYLE_ABXY << 5);
	sys_put_le16(SINPUT_POLLING_RATE_US, &out[8]);
	sys_put_le16(SINPUT_ACCEL_RANGE_G, &out[10]);
	sys_put_le16(SINPUT_GYRO_RANGE_DPS, &out[12]);
	out[14] = 0xFF; /* ABXY + d-pad */
	out[15] = 0xFF; /* L3 R3 LB RB LT RT L4 R4 */
	out[16] = 0xFF; /* Start Back Guide Share L5 R5 LPad RPad */
	out[17] = 0x00;
	out[18] = 2;    /* touchpads */
	out[19] = 1;    /* fingers per pad */

	/* Pseudo-MAC from the unit serial (stable per controller). */
	for(size_t i = 0; i < 6; i++)
	{
		uint8_t v = (uint8_t)(0x5C + i * 17);

		for(size_t j = i; j < serial_len; j += 6)
		{
			v = (uint8_t)((v * 31U) ^ (uint8_t)serial[j]);
		}
		out[20 + i] = v;
	}
}

static void rumble_submit(uint16_t left, uint16_t right)
{
	/* MsgHapticRumble payload (without report ID), same as SDL's Triton driver. */
	uint8_t payload[HID_RUMBLE_OUTPUT_REPORT_BYTES - 1] = { 0 };

	payload[0] = 0;                      /* type */
	sys_put_le16(0, &payload[1]);         /* intensity */
	sys_put_le16(left, &payload[3]);
	payload[5] = 0;                      /* left gain */
	sys_put_le16(right, &payload[6]);
	payload[8] = 0;                      /* right gain */
	(void)haptics_handle_output_report(ID_OUT_REPORT_HAPTIC_RUMBLE, payload, sizeof(payload));
}

static void rumble_work_handler(struct k_work *work)
{
	k_spinlock_key_t key;
	uint16_t left;
	uint16_t right;
	bool expired;

	ARG_UNUSED(work);
	key = k_spin_lock(&rumble_lock);
	expired = (k_uptime_get() - rumble_last_cmd_ms) > SINPUT_RUMBLE_TIMEOUT_MS;
	if(expired)
	{
		rumble_left = 0;
		rumble_right = 0;
	}
	left = rumble_left;
	right = rumble_right;
	k_spin_unlock(&rumble_lock, key);

	rumble_submit(left, right);
	if(left != 0 || right != 0)
	{
		(void)k_work_reschedule(&rumble_work, K_MSEC(SINPUT_RUMBLE_REFRESH_MS));
	}
}

static void rumble_set(uint16_t left, uint16_t right)
{
	k_spinlock_key_t key = k_spin_lock(&rumble_lock);

	rumble_left = left;
	rumble_right = right;
	rumble_last_cmd_ms = k_uptime_get();
	k_spin_unlock(&rumble_lock, key);
	(void)k_work_reschedule(&rumble_work, K_NO_WAIT);
}

static void handle_haptic(const uint8_t *d, size_t len)
{
	if(len < 1)
	{
		return;
	}
	switch(d[0])
	{
		case 1:
			/* Frequency/amplitude pairs: use the stronger amplitude per side. */
			if(len >= 17)
			{
				uint16_t la = MAX(sys_get_le16(&d[3]), sys_get_le16(&d[7]));
				uint16_t ra = MAX(sys_get_le16(&d[11]), sys_get_le16(&d[15]));

				rumble_set(la, ra);
			}
			break;
		case 2:
			/* ERM: amplitude u8 + brake per side. */
			if(len >= 5)
			{
				rumble_set((uint16_t)d[1] * 257U, (uint16_t)d[3] * 257U);
			}
			break;
		default:
			break;
	}
}

size_t sinput_handle_output(uint8_t report_id, const uint8_t *data, size_t len,
                            uint8_t reply[SINPUT_REPORT_SIZE])
{
	const uint8_t *cmd;
	size_t cmd_len;

	if(data == NULL)
	{
		return 0;
	}
	if(report_id == 0)
	{
		/* Interrupt OUT: report ID is the first byte. */
		if(len < 2 || data[0] != SINPUT_REPORT_ID_OUTPUT_CMDDAT)
		{
			return 0;
		}
		cmd = &data[1];
		cmd_len = len - 1;
	}
	else
	{
		if(report_id != SINPUT_REPORT_ID_OUTPUT_CMDDAT)
		{
			return 0;
		}
		/* SET_REPORT may or may not include the ID byte. */
		if(len >= 2 && data[0] == SINPUT_REPORT_ID_OUTPUT_CMDDAT &&
		   (data[1] == SINPUT_CMD_HAPTIC || data[1] == SINPUT_CMD_FEATURES ||
		    data[1] == SINPUT_CMD_PLAYERLED || data[1] == SINPUT_CMD_JOYSTICKRGB))
		{
			cmd = &data[1];
			cmd_len = len - 1;
		}
		else
		{
			cmd = data;
			cmd_len = len;
		}
	}
	if(cmd_len < 1)
	{
		return 0;
	}

	switch(cmd[0])
	{
		case SINPUT_CMD_HAPTIC:
			handle_haptic(&cmd[1], cmd_len - 1);
			return 0;
		case SINPUT_CMD_FEATURES:
			LOG_INF("SInput features requested");
			build_features(reply);
			return SINPUT_REPORT_SIZE;
		case SINPUT_CMD_PLAYERLED:
			return 0;
		case SINPUT_CMD_JOYSTICKRGB:
#if CONFIG_IBEX_RGBW_LED
			if(cmd_len >= 4)
			{
				rgbw_led_set((struct rgbw_color){ cmd[1], cmd[2], cmd[3], 0 });
			}
#endif
			return 0;
		default:
			return 0;
	}
}
