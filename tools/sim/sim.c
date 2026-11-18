/* Host build of the remapper: the real remap.c and sc26re_cfg.c with the rest of
 * the firmware replaced by recording stubs. Driven from Python (ctypes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include "controller.h"
#include "remap.h"
#include "sc26re_cfg.h"
#include "rgbw_led.h"

static int64_t now_ms;
int64_t k_uptime_get(void) { return now_ms; }
uint32_t k_uptime_get_32(void) { return (uint32_t)now_ms; }

/* ---- event log: "K mods k1..k6", "M btn dx dy wh pan", "H level ch", "B freq ms", "L r g b w", "G on" */
static char evlog[1 << 20];
static size_t evlen;
static void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void ev(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = snprintf(evlog + evlen, sizeof(evlog) - evlen, "%lld ", (long long)now_ms);
	if(n > 0) evlen += n;
	n = vsnprintf(evlog + evlen, sizeof(evlog) - evlen, fmt, ap);
	if(n > 0) evlen += n;
	if(evlen < sizeof(evlog) - 1) evlog[evlen++] = '\n';
	evlog[evlen] = 0;
	va_end(ap);
}

int transport_send_input_report(uint8_t id, const uint8_t *d, size_t len)
{
	if(id == 0x41 && len >= 8)
		ev("K %02x %02x %02x %02x %02x %02x %02x", d[0], d[2], d[3], d[4], d[5], d[6], d[7]);
	else if(id == 0x40 && len >= 5)
		ev("M %u %d %d %d %d", d[0], (int8_t)d[1], (int8_t)d[2], (int8_t)d[3], (int8_t)d[4]);
	else
		ev("R %02x %zu", id, len);
	return 0;
}
int haptics_play_feedback(uint8_t level, uint8_t channels) { ev("H %u %u", level, channels); return 0; }
int haptics_play_beep(uint16_t f, uint16_t ms) { ev("B %u %u", f, ms); return 0; }
static struct rgbw_color led;
void rgbw_led_set(struct rgbw_color c) { led = c; ev("L"); }
struct rgbw_color rgbw_led_get(void) { return led; }
static bool gm_on;
void gyro_mouse_set_enabled(bool on) { gm_on = on; ev("G %d", on); }
int32_t gyro_mouse_counts_per_360(void) { return 16364; }
void imu_raw_take(int16_t a[3], int16_t b[3]) { memset(a, 0, 6); memset(b, 0, 6); }
void ibex_settings_notify(uint8_t id) { (void)id; }

/* ---- settings store in memory ---- */
#define NKEYS 512
static struct { char key[64]; uint8_t *v; size_t len; } store[NKEYS];
int settings_save_one(const char *name, const void *value, size_t len)
{
	int fr = -1;
	for(int i = 0; i < NKEYS; ++i)
	{
		if(store[i].v && strcmp(store[i].key, name) == 0) { fr = i; break; }
		if(!store[i].v && fr < 0) fr = i;
	}
	if(fr < 0) return -ENOMEM;
	free(store[fr].v);
	snprintf(store[fr].key, sizeof(store[fr].key), "%s", name);
	store[fr].v = malloc(len ? len : 1);
	memcpy(store[fr].v, value, len);
	store[fr].len = len;
	return 0;
}
int settings_delete(const char *name)
{
	for(int i = 0; i < NKEYS; ++i)
		if(store[i].v && strcmp(store[i].key, name) == 0) { free(store[i].v); store[i].v = NULL; }
	return 0;
}
struct rd { const uint8_t *v; size_t len; };
static ssize_t rd_cb(void *arg, void *data, size_t len)
{
	struct rd *r = arg;
	size_t n = len < r->len ? len : r->len;
	memcpy(data, r->v, n);
	return (ssize_t)n;
}
int settings_load_subtree_direct(const char *sub, settings_load_direct_cb cb, void *param)
{
	size_t sl = strlen(sub);
	for(int i = 0; i < NKEYS; ++i)
	{
		if(!store[i].v || strncmp(store[i].key, sub, sl) != 0 || store[i].key[sl] != '/') continue;
		struct rd r = { store[i].v, store[i].len };
		cb(store[i].key + sl + 1, store[i].len, rd_cb, &r, param);
	}
	return 0;
}

/* ---- API for the driver ---- */
void sim_reset_store(void) { for(int i = 0; i < NKEYS; ++i) { free(store[i].v); store[i].v = NULL; } }
void sim_boot(void) { now_ms = 1000; evlen = 0; evlog[0] = 0; sc26re_cfg_load(); remap_init(); }
void sim_set_time(int64_t ms) { now_ms = ms; }
int64_t sim_time(void) { return now_ms; }
size_t sim_report_size(void) { return sizeof(struct controller_report); }

/* one controller report through the remapper: in -> out (both struct controller_report) */
void sim_step(const uint8_t *in, uint8_t *out, int dt_us)
{
	struct controller_report r;
	memcpy(&r, in, sizeof(r));
	now_ms += 0; /* time advanced by the driver (sub-ms steps) */
	remap_raw_capture(&r);
	remap_process(&r, (float)dt_us * 1e-6f);
	memcpy(out, &r, sizeof(r));
}
const char *sim_events(void) { return evlog; }
void sim_clear_events(void) { evlen = 0; evlog[0] = 0; }
int sim_gyro_mouse(void) { return gm_on; }
uint32_t sim_xbox_buttons(void) { return remap_xbox_buttons(); }
uint8_t sim_mouse_buttons(void) { return remap_mouse_buttons(); }

/* feature report like the firmware: req[0]=op, req[1]=len, path\0data. Returns reply length in rsp. */
int sim_feature(const uint8_t *req, uint8_t *rsp)
{
	char path[64];
	uint8_t op = req[0];
	size_t n = req[1];
	size_t pl = strnlen((const char *)&req[2], n);
	if(pl >= sizeof(path)) return -1;
	memcpy(path, &req[2], pl);
	path[pl] = 0;
	const uint8_t *data = &req[2 + pl + 1];
	size_t dlen = n > pl + 1 ? n - pl - 1 : 0;
	memset(rsp, 0, 64);
	rsp[0] = op;
	if(op == 0xED)
	{
		uint8_t tag = 0xA5;
		for(const char *c = path; *c; ++c) tag = (uint8_t)((tag << 1) ^ (tag >> 7) ^ (uint8_t)*c);
		uint8_t *cur = &rsp[3];
		size_t len = 0;
		bool ok = false;
		if(strcmp(path, "sc26re/raw") == 0) { remap_raw_read(cur); len = 40; ok = true; }
		else if(strcmp(path, "sc26re/rmp_out") == 0) { remap_monitor(cur); len = 30; ok = true; }
		else if(remap_path(path)) ok = remap_read(path, cur, 60, &len);
		else if(sc26re_cfg_path(path)) ok = sc26re_cfg_read(path, cur, 60, &len);
		if(ok) { rsp[2] = tag; rsp[1] = (uint8_t)(len + 1); }
		else rsp[1] = 0;
		return rsp[1] + 2;
	}
	int err = -2;
	if(op == 0xEE)
	{
		if(remap_path(path)) err = remap_stage(path, data, dlen);
		else if(sc26re_cfg_path(path)) err = sc26re_cfg_stage(path, data, dlen);
	}
	else if(op == 0xEF)
	{
		if(remap_path(path)) err = remap_commit(path);
		else if(sc26re_cfg_path(path)) err = sc26re_cfg_commit(path);
	}
	else if(op == 0xF0)
	{
		err = 0; /* calibration commands: accepted, nothing to do here */
	}
	return err;
}

/* gyro range the IMU would use now (profile override or global) */
int sim_gyro_fsr(void)
{
	int16_t v;

	if(!remap_gyro_value(SC26RE_CFG_GYRO_FSR, &v))
	{
		v = sc26re_cfg_get(SC26RE_CFG_GYRO_FSR);
	}
	return v;
}
