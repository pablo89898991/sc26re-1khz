/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Hardware test of the remapper: see hwtest.h. Also built into the simulator. */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "hwtest.h"
#include "remap.h"

#ifndef CONFIG_IBEX_BUILD_TIMESTAMP
#define CONFIG_IBEX_BUILD_TIMESTAMP 0 /* the simulator */
#endif

#define HWT_LOG_OFF (HWT_STEP_SIZE * HWT_MAX_STEPS)

static uint8_t *hb;       /* script, then the log (NULL: no test loaded) */
static size_t hb_size;
static uint8_t n_steps;
static uint8_t step;
static bool armed, running, overflow, aborted, final_pending, snapshot, live_pad;
static int64_t t0, step_end, last_move;
static uint16_t log_n, read_off, end_ms;
static uint8_t last_entry[HWT_ENTRY_SIZE];
static uint8_t base_fb;
static int32_t base_mv[3]; /* the monitor's mouse / wheel totals when the script started */

static uint8_t snap_e[HWT_ENTRY_SIZE]; /* snapshot mode: the outputs of the last pass */
static bool snap_have;

static size_t log_cap(void)
{
	return hb_size > HWT_LOG_OFF ? (hb_size - HWT_LOG_OFF) / HWT_ENTRY_SIZE : 0U;
}

static void put_entry(const uint8_t *e)
{
	if(log_n >= log_cap())
	{
		overflow = true;
		return;
	}
	memcpy(&hb[HWT_LOG_OFF + log_n * HWT_ENTRY_SIZE], e, HWT_ENTRY_SIZE);
	memcpy(last_entry, e, HWT_ENTRY_SIZE);
	log_n++;
}

/* snapshot mode: a step ends, its last pass's outputs go in the log */
static void snap_step_end(void)
{
	if(snapshot && snap_have)
	{
		put_entry(snap_e);
		snap_have = false;
	}
}

/* after a run the outputs stay muted a while (v51 test 66): the test profile's toggled keys,
 * mouse buttons and the rest of a big mouse move used to reach the PC between two runs and the
 * page's restore (a random profile's toggled Alt + F4 closed the browser, mouse button 4 went
 * back a page); the next run or HWT_QUIET_MS ends it, the remapper lets go of everything */
#define HWT_QUIET_MS 15000
static int64_t quiet_until;

static void stop(void)
{
	if(running || armed)
	{
		quiet_until = k_uptime_get() + HWT_QUIET_MS;
		remap_hwtest_ended();
	}
	running = false;
	armed = false;
}

bool hwtest_running(void)
{
	return running || (quiet_until != 0 && k_uptime_get() < quiet_until);
}

bool hwtest_pad_quiet(void)
{
	return running && !live_pad;
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
		remap_hwtest_started();
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
		snap_step_end();
		if(++step >= n_steps || step_end - t0 >= (int64_t)HWT_MAX_MS)
		{
			end_ms = (uint16_t)(now_ms - t0);
			stop();
			final_pending = !snapshot; /* the last movement totals still go in the log */
			return;
		}
		step_end += sys_get_le16(&hb[step * HWT_STEP_SIZE]);
	}
	load_step(r, &hb[step * HWT_STEP_SIZE]);
}

void hwtest_log(const struct controller_report *r, int64_t now_ms)
{
	uint8_t mon[30], e[HWT_ENTRY_SIZE];
	bool moved, final = final_pending;

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
		/* one entry per step: the outputs of its last pass (written when the step ends) */
		memcpy(snap_e, e, sizeof(e));
		snap_have = true;
		return;
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
	put_entry(e);
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
		live_pad = len >= 3U && (v[2] & 2U); /* 2: the pad report carries the script (firmware test) */
		snap_have = false;
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

/* main loop timing, since the last read of sc26re/perf */
static uint32_t pf_n, pf_read_sum, pf_remap_sum, pf_remap_max, pf_gap_max, pf_read_max;
static uint32_t pf_gap_hist[6]; /* gaps < 1.2 ms, < 2, < 3, < 5, < 8, longer */

void hwtest_perf(uint32_t read_us, uint32_t remap_us, uint32_t gap_us)
{
	static const uint32_t edge[5] = { 1200, 2000, 3000, 5000, 8000 };
	int b = 0;

	pf_n++;
	pf_read_sum += read_us;
	pf_remap_sum += remap_us;
	pf_read_max = MAX(pf_read_max, read_us);
	pf_remap_max = MAX(pf_remap_max, remap_us);
	pf_gap_max = MAX(pf_gap_max, gap_us);
	while(b < 5 && gap_us >= edge[b])
	{
		b++;
	}
	pf_gap_hist[b]++;
}

#ifndef SC26RE_SIM
/* ---- sc26re/perf3: where the CPU time goes (test builds) ---- */
#include <cmsis_core.h>

static uint32_t p3_n, p3_wall, p3_own;
static int64_t p3_t0;
static uint64_t p3_busy0;
static struct
{
	const struct k_thread *t;
	uint64_t c0;
	uint64_t d;
	int prio;
} p3_th[10];
static int p3_nth;

uint32_t hwtest_dwt(void)
{
	static bool on;

	if(!on)
	{
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CYCCNT = 0;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		on = true;
	}
	return DWT->CYCCNT;
}

static uint32_t im_n, im_t0, im_wall[5], im_own[5];
static uint64_t im_u0;

void hwtest_imu_mark(int m)
{
	k_thread_runtime_stats_t st;
	uint32_t t = hwtest_dwt();

	(void)k_thread_runtime_stats_get(k_current_get(), &st);
	if(m > 0)
	{
		im_wall[m - 1] += t - im_t0;
		im_own[m - 1] += (uint32_t)(st.execution_cycles - im_u0);
	}
	else
	{
		im_n++;
	}
	im_t0 = t;
	im_u0 = st.execution_cycles;
}

/* 11 u32: ticks, then per section wall us x10 / tick, then own run time us x10 / tick:
 * 1 settings + mid read, 2 I2C read, 3 the report + gravity, 4 gyro mouse feed, 5 sending */
static void perf4_read(uint8_t *buf)
{
	uint32_t v[12] = { im_n };
	uint32_t cpu_mhz = SystemCoreClock / 1000000U;

	for(int i = 0; i < 5 && im_n; ++i)
	{
		v[1 + i] = (uint32_t)((uint64_t)im_wall[i] * 10U / cpu_mhz / im_n);
		v[6 + i] = (uint32_t)(k_cyc_to_ns_floor64(im_own[i]) / 100U / im_n);
		im_wall[i] = im_own[i] = 0;
	}
	im_n = 0;
	for(int i = 0; i < 12; ++i)
	{
		sys_put_le32(v[i], &buf[4 * i]);
	}
}

/* 9 u32: ICACHECNF, HFCLKSTAT, cycles / 1000 loop steps with interrupts off, same with them on
 * (x1000 steps), cache hits and misses since the last read, DWT cycles over a 1000 us busy wait,
 * SCB->CPACR, the read's own thread priority */
static void perf5_read(uint8_t *buf)
{
	static bool prof;
	uint32_t v[12] = { 0 };
	volatile uint32_t x = 0;
	uint32_t t, key;

	v[0] = NRF_NVMC->ICACHECNF;
	v[1] = NRF_CLOCK->HFCLKSTAT;
	key = irq_lock();
	t = hwtest_dwt();
	for(int i = 0; i < 1000; ++i)
	{
		x++;
	}
	v[2] = hwtest_dwt() - t;
	irq_unlock(key);
	t = hwtest_dwt();
	for(int i = 0; i < 1000000; ++i)
	{
		x++;
	}
	v[3] = hwtest_dwt() - t;
	v[4] = NRF_NVMC->IHIT;
	v[5] = NRF_NVMC->IMISS;
	if(!prof)
	{
		NRF_NVMC->ICACHECNF |= NVMC_ICACHECNF_CACHEPROFEN_Msk;
		prof = true;
	}
	NRF_NVMC->IHIT = 0;
	NRF_NVMC->IMISS = 0;
	t = hwtest_dwt();
	k_busy_wait(1000);
	v[6] = hwtest_dwt() - t;
	v[7] = SCB->CPACR;
	v[8] = (uint32_t)(k_thread_priority_get(k_current_get()) + 64);
	for(int i = 0; i < 12; ++i)
	{
		sys_put_le32(v[i], &buf[4 * i]);
	}
}

void hwtest_perf3(uint32_t wall_cyc, uint32_t own_cyc)
{
	p3_n++;
	p3_wall += wall_cyc;
	p3_own += own_cyc;
}

static void p3_thread(const struct k_thread *t, void *arg)
{
	k_thread_runtime_stats_t st;
	bool snap = arg != NULL;
	int i;

	if(k_thread_runtime_stats_get((k_tid_t)t, &st) != 0)
	{
		return;
	}
	for(i = 0; i < p3_nth && p3_th[i].t != t; ++i)
	{
	}
	if(i == p3_nth)
	{
		if(p3_nth >= (int)ARRAY_SIZE(p3_th))
		{
			return;
		}
		p3_nth++;
		p3_th[i].t = t;
		p3_th[i].c0 = st.execution_cycles;
	}
	p3_th[i].prio = t->base.prio;
	p3_th[i].d = snap ? 0 : st.execution_cycles - p3_th[i].c0;
	p3_th[i].c0 = st.execution_cycles;
}

/* 12 u32: passes, remapper wall us / pass (x10), its own main-thread run time / pass (x10),
 * window ms, non-idle us in the window, then the 7 busiest threads as prio << 24 | us */
static void perf3_read(uint8_t *buf)
{
	k_thread_runtime_stats_t all;
	int64_t now = k_uptime_get();
	uint32_t v[12] = { 0 };
	uint32_t cpu_mhz = SystemCoreClock / 1000000U;

	for(int i = 0; i < p3_nth; ++i)
	{
		p3_th[i].d = 0;
	}
	k_thread_foreach_unlocked(p3_thread, NULL);
	(void)k_thread_runtime_stats_all_get(&all);
	v[0] = p3_n;
	v[1] = p3_n ? (uint32_t)((uint64_t)p3_wall * 10U / cpu_mhz / p3_n) : 0;
	v[2] = p3_n ? (uint32_t)(k_cyc_to_ns_floor64(p3_own) / 100U / p3_n) : 0;
	v[3] = (uint32_t)(now - p3_t0);
	v[4] = (uint32_t)k_cyc_to_us_floor64(all.total_cycles - p3_busy0);
	for(int k = 0; k < 7; ++k)
	{
		int best = -1;

		for(int i = 0; i < p3_nth; ++i)
		{
			if(p3_th[i].d != 0 && (best < 0 || p3_th[i].d > p3_th[best].d))
			{
				best = i;
			}
		}
		if(best < 0)
		{
			break;
		}
		v[5 + k] = ((uint32_t)(uint8_t)(p3_th[best].prio + 64) << 24) |
		           MIN((uint32_t)k_cyc_to_us_floor64(p3_th[best].d), 0xFFFFFFU);
		p3_th[best].d = 0;
	}
	for(int i = 0; i < 12; ++i)
	{
		sys_put_le32(v[i], &buf[4 * i]);
	}
	p3_n = p3_wall = p3_own = 0;
	p3_t0 = now;
	p3_busy0 = all.total_cycles;
}
#else
uint32_t hwtest_dwt(void) { return 0; }
void hwtest_perf3(uint32_t wall_cyc, uint32_t own_cyc) { (void)wall_cyc; (void)own_cyc; }
void hwtest_imu_mark(int m) { (void)m; }
#endif

bool hwtest_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
#ifndef SC26RE_SIM
	if(strcmp(path, "sc26re/perf5") == 0 && capacity >= 48)
	{
		perf5_read(buf);
		*len = 48;
		return true;
	}
	if(strcmp(path, "sc26re/perf4") == 0 && capacity >= 48)
	{
		perf4_read(buf);
		*len = 48;
		return true;
	}
	if(strcmp(path, "sc26re/perf3") == 0 && capacity >= 48)
	{
		perf3_read(buf);
		*len = 48;
		return true;
	}
#endif
#ifndef SC26RE_SIM
	if(strcmp(path, "sc26re/perf2") == 0 && capacity >= 48)
	{
		/* the remapper pass by section, us summed since the last read; then the bindings
		 * loop's lookups (binding() + bound()) in us and the bound sources counted */
		extern void remap_perf_get(uint32_t out[12]);
		uint32_t v[12];

		remap_perf_get(v);
		for(int i = 0; i < 12; ++i)
		{
			sys_put_le32(v[i], &buf[4 * i]);
		}
		*len = 48;
		return true;
	}
#endif
	if(strcmp(path, "sc26re/perf") == 0 && capacity >= 48)
	{
		/* passes, mean / max of the scan (incl. its wait) and the remapper, the longest gap,
		 * gap histogram; reading starts a new window */
		uint32_t v[12] = { pf_n, pf_n ? pf_read_sum / pf_n : 0, pf_read_max, pf_n ? pf_remap_sum / pf_n : 0, pf_remap_max,
		                   pf_gap_max, pf_gap_hist[0], pf_gap_hist[1], pf_gap_hist[2], pf_gap_hist[3], pf_gap_hist[4],
		                   pf_gap_hist[5] };

		for(int i = 0; i < 12; ++i)
		{
			sys_put_le32(v[i], &buf[4 * i]);
		}
		pf_n = pf_read_sum = pf_remap_sum = pf_remap_max = pf_gap_max = pf_read_max = 0;
		memset(pf_gap_hist, 0, sizeof(pf_gap_hist));
		*len = 48;
		return true;
	}
	if(strcmp(path, "sc26re/hwt_st") == 0 && capacity >= 10)
	{
		buf[0] = (uint8_t)((hb ? 1U : 0U) | (armed ? 2U : 0U) | (running ? 4U : 0U) | (overflow ? 8U : 0U) |
		                   (aborted ? 16U : 0U));
		buf[1] = step;
		sys_put_le16(log_n, &buf[2]);
		sys_put_le16(end_ms, &buf[4]);
		sys_put_le16((uint16_t)log_cap(), &buf[6]);
		buf[8] = HWT_MAX_STEPS;
		buf[9] = 4; /* version: 2 = gyro / accel in the steps; Steam pad, Xbox analog, haptics, profile in the log; snapshots; 3 = a snapshot at the end of every step */
		*len = 10;
		return true;
	}
	if(strcmp(path, "sc26re/fwid") == 0 && capacity >= 5)
	{
		/* which build this is (the page cannot tell test builds apart otherwise) */
		sys_put_le32((uint32_t)CONFIG_IBEX_BUILD_TIMESTAMP, &buf[0]);
		buf[4] = 4; /* hardware test version */
		*len = 5;
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
