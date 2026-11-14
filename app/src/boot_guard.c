/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "boot_guard.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "power.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(boot_guard, LOG_LEVEL_INF);

/* Retained word next to the OFW-compatible continue-wireless magic (0x2001ffd4): outside
 * Zephyr RAM and not used by the Valve bootloader. */
#define BOOT_GUARD_WORD (*(volatile uint32_t *)0x2001ffc8u)
#define BOOT_GUARD_MAGIC 0xb0070000u
#define BOOT_GUARD_MAGIC_MASK 0xffff0000u
#define BOOT_GUARD_FATAL_FLAG 0x100u
#define BOOT_GUARD_FEED_MS 1000
#define BOOT_GUARD_MAX_FEED_MS 30000
#define BOOT_GUARD_STABLE_MS 10000
#define BOOT_GUARD_SAFE_ATTEMPTS 3U
#define BOOT_GUARD_GIVE_UP_ATTEMPTS 5U

static uint8_t attempts;
static int64_t loop_since_ms = -1;
static bool cleared;

static void boot_guard_feed(struct k_timer *timer)
{
	if(k_uptime_get() > BOOT_GUARD_MAX_FEED_MS)
	{
		/* something is really stuck: let the watchdog reset it */
		k_timer_stop(timer);
		return;
	}
	watchdog_feed();
}

static K_TIMER_DEFINE(boot_guard_timer, boot_guard_feed, NULL);

static void boot_guard_store(uint8_t count)
{
	BOOT_GUARD_WORD = count == 0U ? 0U : (BOOT_GUARD_MAGIC | count);
}

void boot_guard_begin(void)
{
	uint32_t word = BOOT_GUARD_WORD;
	bool valid = (word & BOOT_GUARD_MAGIC_MASK) == BOOT_GUARD_MAGIC;
	/* only a hang (watchdog), a lockup or our own retry reboot count as a failed boot;
	 * power-on, Steam-button wake and normal reboots start the count over */
	bool failed = (power_boot_reset_reason() &
	               (POWER_RESETREAS_DOG_Msk | POWER_RESETREAS_LOCKUP_Msk)) != 0U ||
	              (valid && (word & BOOT_GUARD_FATAL_FLAG) != 0U);

	attempts = valid && failed ? (uint8_t)(word & 0xffU) : 0U;
	if(attempts < UINT8_MAX)
	{
		attempts++;
	}
	boot_guard_store(attempts);
	watchdog_feed();
	k_timer_start(&boot_guard_timer, K_MSEC(BOOT_GUARD_FEED_MS), K_MSEC(BOOT_GUARD_FEED_MS));
	if(attempts > 1U)
	{
		LOG_WRN("boot attempt %u without a stable start before", attempts);
	}
}

uint8_t boot_guard_attempts(void)
{
	return attempts;
}

bool boot_guard_safe_mode(void)
{
	return attempts >= BOOT_GUARD_SAFE_ATTEMPTS;
}

void boot_guard_loop(void)
{
	int64_t now;

	if(cleared)
	{
		return;
	}
	now = k_uptime_get();
	if(loop_since_ms < 0)
	{
		/* main loop reached: it feeds the watchdog itself from here on */
		loop_since_ms = now;
		k_timer_stop(&boot_guard_timer);
		return;
	}
	if(now - loop_since_ms >= BOOT_GUARD_STABLE_MS)
	{
		cleared = true;
		boot_guard_store(0U);
	}
}

void boot_guard_fatal(const char *stage, int err)
{
	LOG_ERR("%s failed: %d (boot attempt %u)", stage, err, attempts);
	if(attempts < BOOT_GUARD_GIVE_UP_ATTEMPTS)
	{
		BOOT_GUARD_WORD = BOOT_GUARD_MAGIC | BOOT_GUARD_FATAL_FLAG | attempts;
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
	}
	/* give up without looping: clean power-off; the next Steam press starts fresh */
	boot_guard_store(0U);
	power_off_quiet();
	for(;;)
	{
		k_msleep(500);
		watchdog_feed();
	}
}
