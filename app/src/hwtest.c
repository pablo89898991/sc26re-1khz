/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Hardware test of the remapper: see hwtest.h. Also built into the simulator. */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "hwtest.h"
#include "remap.h"

#define HWT_LOG_OFF (HWT_STEP_SIZE * HWT_MAX_STEPS)

static uint8_t *hb;       /* script, then the log (NULL: no test loaded) */
static size_t hb_size;
static uint8_t n_steps;
static uint8_t step;
static bool armed, running, overflow, aborted, final_pending, snapshot;
static uint8_t snap_step;
static int64_t t0, step_end, last_move;
static uint16_t log_n, read_off, end_ms;
static uint8_t last_entry[HWT_ENTRY_SIZE];
static uint8_t base_fb;
static int32_t base_mv[3]; /* the monitor's mouse / wheel totals when the script started */

static size_t log_cap(void)
{
	return hb_size > HWT_LOG_OFF ? (hb_size - HWT_LOG_OFF) / HWT_ENTRY_SIZE : 0U;
}

static void stop(void)
{
	running = false;
	armed = false;
}

bool hwtest_running(void)
{
	return running;
}

static void load_step(struct controller_report *r, const uint8_t *s)
{
	r->buttons = sys_get_le32(&s[2]);
	r->trigger_left = (int16_t)sys_get_le16(&s[6]);
	r->trigger_right = (int16_t)sys_get_le16(&s[8]);
	r->stick_left_x = (int16_t)sys_get_le16(&s[10]);
	r->stick_left_y = (int16_t)sys_get_le16(&s[12]);
	r->stick_right_x = (int16_t)sys_get_le16(&s[14]);
	r->stick_right_y = (int16_t)sys_get_le16(&s[16]);
	r->touchpad_left_x = (int16_t)sys_get_le16(&s[18]);
	r->touchpad_left_y = (int16_t)sys_get_le16(&s[20]);
	r->touchpad_left_pressure = sys_get_le16(&s[22]);
	r->touchpad_right_x = (int16_t)sys_get_le16(&s[24]);
	r->touchpad_right_y = (int16_t)sys_get_le16(&s[26]);
	r->touchpad_right_pressure = sys_get_le16(&s[28]);
	r->gyro_x = (int16_t)sys_get_le16(&s[30]);
	r->gyro_y = (int16_t)sys_get_le16(&s[32]);
	r->gyro_z = (int16_t)sys_get_le16(&s[34]);
	r->accel_x = (int16_t)sys_get_le16(&s[36]);
	r->accel_y = (int16_t)sys_get_le16(&s[38]);
	r->accel_z = (int16_t)sys_get_le16(&s[40]);
}

void hwtest_apply(struct controller_report *r, int64_t now_ms)
{
	if(!armed && !running)
	{
		return;
	}
	if(hb == NULL || (r->buttons & BIT(CONTROLLER_BUTTON_STEAM)))
	{
		/* the physical Steam button (or a profile upload that took the buffer) ends it */
		aborted = hb != NULL;
		stop();
		return;
	}
	if(armed)
	{
		armed = false;
		running = true;
		step = 0;
		t0 = now_ms;
		step_end = now_ms + sys_get_le16(&hb[0]);
		last_move = now_ms;
		{
			uint8_t mon[30];

			remap_monitor(mon);
			base_mv[0] = (int32_t)sys_get_le32(&mon[20]);
			base_mv[1] = (int32_t)sys_get_le32(&mon[24]);
			base_mv[2] = (int16_t)sys_get_le16(&mon[28]);
			base_fb = remap_haptic_count();
		}
	}
	while(running && now_ms >= step_end)
	{
		if(++step >= n_steps || step_end - t0 >= (int64_t)HWT_MAX_MS)
		{
			end_ms = (uint16_t)(now_ms - t0);
			stop();
			final_pending = true; /* the last movement totals still go in the log */
			return;
		}
		step_end += sys_get_le16(&hb[step * HWT_STEP_SIZE]);
	}
	load_step(r, &hb[step * HWT_STEP_SIZE]);
}

void hwtest_log(const struct controller_report *r, int64_t now_ms)
{
	uint8_t mon[30], e[HWT_ENTRY_SIZE];
	bool moved, final = final_pending, snap_now = false;

	final_pending = false;
	if((!running && !final) || hb == NULL)
	{
		return;
	}
	remap_monitor(mon);
	sys_put_le16((uint16_t)(now_ms - t0), &e[0]);
	e[2] = mon[3];               /* mouse buttons */
	e[3] = mon[2];               /* shifted groups */
	memcpy(&e[4], &mon[4], 8);   /* keyboard report */
	sys_put_le32(r->buttons, &e[12]); /* the Steam pad's buttons after the remapper */
	memcpy(&e[16], &mon[16], 4); /* Xbox buttons */
	/* mouse dx, dy, wheel: totals since the script started */
	sys_put_le32((uint32_t)((int32_t)sys_get_le32(&mon[20]) - base_mv[0]), &e[20]);
	sys_put_le32((uint32_t)((int32_t)sys_get_le32(&mon[24]) - base_mv[1]), &e[24]);
	sys_put_le16((uint16_t)((int16_t)sys_get_le16(&mon[28]) - base_mv[2]), &e[28]);
	e[30] = remap_xbox_trigger(0);
	e[31] = remap_xbox_trigger(1);
	for(int i = 0; i < 2; ++i)
	{
		int16_t x, y;

		remap_xbox_stick(i, &x, &y);
		e[32 + 2 * i] = (uint8_t)(int8_t)(x / 256);
		e[33 + 2 * i] = (uint8_t)(int8_t)(y / 256);
	}
	{
		uint16_t ax, ay;

		remap_abs_pos(&ax, &ay); /* mouse region: the absolute cursor */
		sys_put_le16(ax, &e[36]);
		sys_put_le16(ay, &e[38]);
	}
	e[40] = (uint8_t)(remap_haptic_count() - base_fb);
	e[41] = mon[0]; /* active profile */
	/* the Steam pad's sticks and triggers after the remapper */
	sys_put_le16((uint16_t)r->stick_left_x, &e[42]);
	sys_put_le16((uint16_t)r->stick_left_y, &e[44]);
	sys_put_le16((uint16_t)r->stick_right_x, &e[46]);
	sys_put_le16((uint16_t)r->stick_right_y, &e[48]);
	sys_put_le16((uint16_t)r->trigger_left, &e[50]);
	sys_put_le16((uint16_t)r->trigger_right, &e[52]);
	if(snapshot)
	{
		/* one entry at the end of each step (and the last state) */
		snap_now = final || (running && step != snap_step && now_ms >= step_end - 2);
		if(!snap_now)
		{
			return;
		}
		snap_step = step;
	}
	else
	{
		/* analog outputs alone (mouse, wheel, Xbox and Steam sticks / triggers): at most every HWT_MOVE_MS */
		moved = log_n > 0 && (memcmp(&e[20], &last_entry[20], 20) != 0 || memcmp(&e[42], &last_entry[42], 12) != 0);
		if(log_n > 0 && memcmp(&e[2], &last_entry[2], 18) == 0 && memcmp(&e[40], &last_entry[40], 2) == 0 &&
		   !(moved && (final || now_ms - last_move >= HWT_MOVE_MS)))
		{
			return;
		}
		if(moved)
		{
			last_move = now_ms;
		}
	}
	if(log_n >= log_cap())
	{
		overflow = true;
		return;
	}
	memcpy(&hb[HWT_LOG_OFF + log_n * HWT_ENTRY_SIZE], e, sizeof(e));
	memcpy(last_entry, e, sizeof(e));
	log_n++;
}

void hwtest_buffer_lost(void)
{
	aborted = running || armed;
	stop();
	hb = NULL;
}

int hwtest_set(const uint8_t *v, size_t len)
{
	if(len < 1U)
	{
		return -EINVAL;
	}
	switch(v[0])
	{
	case 1: /* script bytes: off u16, data */
	{
		uint16_t off;

		if(len < 3U)
		{
			return -EINVAL;
		}
		if(running || armed)
		{
			return -EBUSY;
		}
		if(hb == NULL)
		{
			hb = remap_hwtest_buffer(&hb_size);
			if(hb == NULL)
			{
				return -EBUSY;
			}
		}
		off = sys_get_le16(&v[1]);
		if((size_t)off + (len - 3U) > HWT_LOG_OFF)
		{
			return -EINVAL;
		}
		memcpy(&hb[off], &v[3], len - 3U);
		return 0;
	}
	case 2: /* start: number of steps */
		if(hb == NULL || len < 2U || v[1] == 0U || v[1] > HWT_MAX_STEPS || running || armed)
		{
			return -EINVAL;
		}
		n_steps = v[1];
		snapshot = len >= 3U && (v[2] & 1U); /* 1: one entry at the end of each step */
		snap_step = 0xFF;
		log_n = 0;
		read_off = 0;
		end_ms = 0;
		overflow = aborted = final_pending = false;
		armed = true;
		return 0;
	case 3: /* stop, give the buffer back */
		stop();
		hb = NULL;
		remap_hwtest_release();
		return 0;
	case 4: /* log read position (entries) */
		if(len < 3U)
		{
			return -EINVAL;
		}
		read_off = sys_get_le16(&v[1]);
		return 0;
	default:
		return -EINVAL;
	}
}

bool hwtest_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
	if(strcmp(path, "sc26re/hwt_st") == 0 && capacity >= 10)
	{
		buf[0] = (uint8_t)((hb ? 1U : 0U) | (armed ? 2U : 0U) | (running ? 4U : 0U) | (overflow ? 8U : 0U) |
		                   (aborted ? 16U : 0U));
		buf[1] = step;
		sys_put_le16(log_n, &buf[2]);
		sys_put_le16(end_ms, &buf[4]);
		sys_put_le16((uint16_t)log_cap(), &buf[6]);
		buf[8] = HWT_MAX_STEPS;
		buf[9] = 2; /* version: 2 = gyro / accel in the steps; Steam pad, Xbox analog, haptics, profile in the log; snapshots */
		*len = 10;
		return true;
	}
	if(strcmp(path, "sc26re/hwt_log") == 0 && capacity >= 2U + HWT_ENTRY_SIZE)
	{
		/* one entry per read, after its index: a read the page repeats (reply lost) shows up
		 * as the next index, and the page sets the position back */
		if(hb == NULL || running || read_off >= log_n)
		{
			*len = 0;
			return true;
		}
		sys_put_le16(read_off, &buf[0]);
		memcpy(&buf[2], &hb[HWT_LOG_OFF + read_off * HWT_ENTRY_SIZE], HWT_ENTRY_SIZE);
		read_off++;
		*len = 2U + HWT_ENTRY_SIZE;
		return true;
	}
	return false;
}
