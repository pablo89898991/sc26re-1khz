#include <stdint.h>
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
	char line[160];
	int n0, n1;

	if(evlen > sizeof(evlog) - 200)
	{
		evlen = 0; /* the driver reads the log after every run(); keep the newest */
	}
	n0 = snprintf(line, sizeof(line), "%lld ", (long long)now_ms);
	va_start(ap, fmt);
	n1 = vsnprintf(line + n0, sizeof(line) - (size_t)n0, fmt, ap);
	va_end(ap);
	if(n0 < 0 || n1 < 0) return;
	size_t n = strnlen(line, sizeof(line) - 1);
	memcpy(evlog + evlen, line, n);
	evlen += n;
	evlog[evlen++] = '\n';
	evlog[evlen] = 0;
}

/* ---- transport model: what the host (PC) actually receives ----
 * 0 USB: every report arrives.
 * 1 Puck (ESB, as transport_esb.c): queue of 5, mouse movement merges into a waiting
 *   mouse report, -EAGAIN when full; one payload per 1 ms frame carries the queued
 *   reports (esb_one = 0: all that fit, like the firmware; 1: one per payload); the
 *   Puck turns a payload into one USB report: only the newest of them survives.
 * 2 Bluetooth (as transport_ble.c): queue of 16 (-ENOMEM when full), drained into
 *   bt_gatt_notify while the stack has buffers; buffers free at each connection event.
 * Link trouble: frames lost (bursts) and disconnects (queue dropped). */
struct qrep { uint8_t id, len, d[8]; uint8_t state; };
static int state_pending;      /* keyboard / mouse-button reports not yet delivered (as the firmware counts them) */
static int64_t state_left_ms;
static uint8_t last_mb_queued;
static int tmode, esb_one, ble_bufs = 3, ble_ci = 15, link_up = 1;
static unsigned rng = 1;
static int lossy_pct, burst_left;
static struct qrep q[16];
static int qn;
static int64_t q_empty_ms;
static uint8_t host_kbd[8], host_mb;
static long host_dx, host_dy, host_wh, host_pan;
static int host_lost;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void host_deliver(const struct qrep *r)
{
	if(r->id == 0x41)
	{
		if(memcmp(host_kbd, r->d, 8) != 0)
		{
			memcpy(host_kbd, r->d, 8);
			ev("HK %02x %02x %02x %02x %02x %02x %02x", r->d[0], r->d[2], r->d[3], r->d[4], r->d[5], r->d[6], r->d[7]);
		}
	}
	else if(r->id == 0x40)
	{
		host_mb = r->d[0];
		host_dx += (int8_t)r->d[1]; host_dy += (int8_t)r->d[2]; host_wh += (int8_t)r->d[3]; host_pan += (int8_t)r->d[4];
		ev("HM %u %d %d %d %d", r->d[0], (int8_t)r->d[1], (int8_t)r->d[2], (int8_t)r->d[3], (int8_t)r->d[4]);
	}
}

static int tsend(uint8_t id, const uint8_t *d, size_t len);
int transport_send_input_report(uint8_t id, const uint8_t *d, size_t len)
{
	int err = tsend(id, d, len);

	if(err == 0) /* the device log: only reports the transport took */
	{
		if(id == 0x41 && len >= 8)
			ev("K %02x %02x %02x %02x %02x %02x %02x", d[0], d[2], d[3], d[4], d[5], d[6], d[7]);
		else if(id == 0x40 && len >= 5)
			ev("M %u %d %d %d %d", d[0], (int8_t)d[1], (int8_t)d[2], (int8_t)d[3], (int8_t)d[4]);
	}
	return err;
}

static int tsend(uint8_t id, const uint8_t *d, size_t len)
{
	struct qrep r = { id, (uint8_t)len };
	memcpy(r.d, d, len < 8 ? len : 8);
	if(tmode == 0) { host_deliver(&r); return 0; }
	if(!link_up) return -ENOTCONN;
	if(tmode == 1)
	{
		if(id == 0x40)
		{
			/* as transport_esb.c: the newest mouse report decides, never the head */
			for(int n = qn - 1; n >= (esb_one ? 1 : 0); --n)
			{
				if(q[n].id != 0x40) continue;
				int ok = !esb_one || q[n].d[0] == d[0];
				for(int i = 1; i < 5; ++i) { int v = (int8_t)q[n].d[i] + (int8_t)d[i]; ok = ok && v >= -127 && v <= 127; }
				if(ok)
				{
					q[n].d[0] = d[0];
					for(int i = 1; i < 5; ++i) q[n].d[i] = (uint8_t)(int8_t)((int8_t)q[n].d[i] + (int8_t)d[i]);
					return 0;
				}
				if(esb_one) break;
			}
		}
		if(qn == 5) return -EAGAIN;
		r.state = id == 0x41 || (id == 0x40 && d[0] != last_mb_queued);
		if(id == 0x40) last_mb_queued = d[0];
		state_pending += r.state;
		q[qn++] = r;
		return 0;
	}
	if(qn == 16) return -ENOMEM;
	r.state = id == 0x41 || (id == 0x40 && d[0] != last_mb_queued);
	if(!r.state && qn >= 2) return -EAGAIN; /* as transport_ble.c: movement never fills the queue */
	if(id == 0x40) last_mb_queued = d[0];
	state_pending += r.state;
	q[qn++] = r;
	return 0;
}

static struct qrep stk[8];
static int stk_n;
/* as transport.c: ms since the mouse / keyboard queue emptied, USB: always */
int32_t transport_input_idle_ms(void)
{
	if(tmode == 0) return INT32_MAX;
	return state_pending > 0 ? 0 : (int32_t)(now_ms - state_left_ms);
}
/* one millisecond of radio */
static void transport_tick(void)
{
	if(tmode == 0 || !link_up) return;
	bool lost = false;
	if(burst_left > 0) { burst_left--; lost = true; }
	else if((int)(rnd() % 1000) < lossy_pct) { burst_left = (int)(rnd() % 40); lost = true; }
	if(tmode == 1)
	{
		if(lost || qn == 0) return;
		int take = esb_one ? 1 : qn; /* 128-byte payload: all 5 fit */
		host_lost += take - 1;
		host_deliver(&q[take - 1]); /* the Puck keeps the newest */
		for(int i = 0; i < take; ++i) if(q[i].state) { state_pending--; state_left_ms = now_ms; } /* left in a payload */
		memmove(q, q + take, (size_t)(qn - take) * sizeof(q[0]));
		qn -= take;
		if(qn == 0) q_empty_ms = now_ms;
		return;
	}
	/* Bluetooth: the queue empties into the stack's buffers at once (that is
	 * when transport_ble sees it empty); the buffers go on air at each
	 * connection event */
	while(qn > 0 && stk_n < ble_bufs)
	{
		stk[stk_n++] = q[0];
		memmove(q, q + 1, (size_t)(qn - 1) * sizeof(q[0]));
		qn--;
		if(qn == 0) q_empty_ms = now_ms;
	}
	if(now_ms % ble_ci == 0 && !lost)
	{
		for(int i = 0; i < stk_n; ++i) { host_deliver(&stk[i]); if(stk[i].state) { state_pending--; state_left_ms = now_ms; } } /* sent callback */
		stk_n = 0;
	}
}

void sim_transport(int mode, int one, int loss_permille, int ci_ms, unsigned seed)
{
	tmode = mode; esb_one = one; lossy_pct = loss_permille; ble_ci = ci_ms > 0 ? ci_ms : 15; rng = seed | 1;
}
void sim_link(int up)
{
	if(up == link_up) return;
	link_up = up;
	if(!up)
	{
		qn = 0; /* teardown drops what was waiting */
		q_empty_ms = now_ms;
		state_pending = 0; state_left_ms = now_ms;
		if(tmode == 2) { stk_n = 0; state_pending = 0; memset(host_kbd, 0, 8); host_mb = 0; ev("HK 00 00 00 00 00 00 00"); } /* BT host releases */
	}
}
void sim_host(int32_t *out)
{
	out[0] = host_mb; out[1] = (int32_t)host_dx; out[2] = (int32_t)host_dy; out[3] = (int32_t)host_wh; out[4] = (int32_t)host_pan;
	out[5] = host_lost; out[6] = qn + stk_n;
	memcpy(&out[7], host_kbd, 8);
}
int haptics_play_feedback(uint8_t level, uint8_t channels) { ev("H %u %u", level, channels); return 0; }
int haptics_play_beep(uint16_t f, uint16_t ms) { ev("B %u %u", f, ms); return 0; }
static struct rgbw_color led;
void rgbw_led_set(struct rgbw_color c) { led = c; ev("L %u %u %u", c.r, c.g, c.b); }
struct rgbw_color rgbw_led_get(void) { return led; }
static bool gm_on;
#ifndef WITH_GM
void gyro_mouse_set_enabled(bool on) { gm_on = on; (void)sc26re_cfg_set(SC26RE_CFG_GM_ENABLED, on ? 1 : 0, false); ev("G %d", on); } /* as gyro_mouse.c */
int32_t gyro_mouse_counts_per_360(void) { return 16364; }
#endif
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
#ifdef WITH_GM
#include "gyro_mouse.h"
static float gm_dps[3];
static struct controller_report gm_state; /* hardware_store_state() */
static bool gm_state_valid;
static int gm_physical; /* 1: feed the physical buttons (fixed firmware), 0: the stored state (as before) */
void sim_gyro(float x, float y, float z) { gm_dps[0] = x; gm_dps[1] = y; gm_dps[2] = z; }
void sim_gm_physical(int on) { gm_physical = on; }
#endif

void sim_step(const uint8_t *in, uint8_t *out, int dt_us)
{
	struct controller_report r;
	memcpy(&r, in, sizeof(r));
#ifdef WITH_GM
	{
		/* imu.c: every gyro sample feeds the gyro mouse, its counts go out as mouse reports */
		uint32_t b = gm_physical ? r.buttons : (gm_state_valid ? remap_raw_buttons() : 0U); /* as imu.c now */
		int8_t dx, dy;

		for(int t = 0; t < (dt_us + 999) / 1000; ++t) gyro_mouse_feed(gm_dps, b, 0.001f);
		if(gyro_mouse_has_counts() && gyro_mouse_take(&dx, &dy))
		{
			uint8_t m[5] = { 0, (uint8_t)dx, (uint8_t)dy, 0, 0 };
			int e;

			remap_mouse_out_lock(); /* as imu.c */
			m[0] = remap_mouse_buttons();
			e = transport_send_input_report(0x40, m, sizeof(m));
			remap_mouse_out_unlock();
			if(e != 0) gyro_mouse_give_back(dx, dy);
			else gyro_mouse_note_sent(dx, dy);
		}
	}
#endif
	now_ms += 0; /* time advanced by the driver (sub-ms steps) */
	for(int t = 0; t < (dt_us + 999) / 1000; ++t) transport_tick();
	remap_raw_capture(&r);
	remap_process(&r, (float)dt_us * 1e-6f);
#ifdef WITH_GM
	gyro_mouse_joystick(&r);              /* main.c order */
	r.buttons = gyro_mouse_filter_buttons(r.buttons);
	gm_state = r; gm_state_valid = true;  /* hardware_store_state */
#endif
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
		if(strcmp(path, "sc26re/awake") == 0) { cur[0] = 1; len = 1; ok = true; } /* meter keep-alive, as valve_settings.c */
		else if(strcmp(path, "sc26re/raw") == 0) { remap_raw_read(cur); len = 40; ok = true; }
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

/* profile gyro value k (index in remap_gyro_keys) the gyro code would see: -32768 = global */
int sim_gyro_key(int k)
{
	int16_t v;

	return remap_gyro_value(remap_gyro_keys[k], &v) ? v : -32768;
}
void sim_led(uint8_t *out) { out[0] = led.r; out[1] = led.g; out[2] = led.b; }
const uint8_t *remap_sim_staging(int *p);
int sim_staging(uint8_t *out) { int p; const uint8_t *b = remap_sim_staging(&p); memcpy(out, b, 2056); return p; }
void remap_sim_mouse(float *out);
void sim_mouse_state(float *out) { remap_sim_mouse(out); }
