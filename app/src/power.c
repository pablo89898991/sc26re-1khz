/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#if CONFIG_POWEROFF
#include <zephyr/sys/poweroff.h>
#endif
#include <zephyr/sys/reboot.h>
#include <zephyr/fatal.h>
#include <zephyr/linker/section_tags.h>
#include <string.h>

#include "analog.h"
#include "battery.h"
#include "controller.h"
#include "power.h"
#include "remap.h"
#include "haptics.h"
#include "puck_interface.h"
#include "rgbw_led.h"
#include "watchdog.h"

#define VALVE_ISP_MAGIC_BASE 0x2001fff0u
#define CONTINUE_WIRELESS_MAGIC 0xd5cee019u

/* Same retained word as OFW (0x2001ff00 + 0xd4), outside Zephyr RAM and untouched by the bootloader. */
#define continue_wireless_magic (*(volatile uint32_t *)0x2001ffd4u)
#define POWER_OFF_RELEASE_POLL_MS 20
#define POWER_OFF_RELEASE_DEBOUNCE_MS 100
#define POWER_OFF_EXTERNAL_POLL_MS 100
#define POWER_OFF_THREAD_STACK_SIZE 1024
#define POWER_OFF_THREAD_PRIORITY K_PRIO_COOP(9)
#define NRF_GPIO_PORT_COUNT 2
#define NRF_GPIO_PINS_PER_PORT 32
#define STEAM_BUTTON_NODE DT_ALIAS(button_steam)
#define STEAM_BUTTON_MASK BIT(CONTROLLER_BUTTON_STEAM)

#if DT_NODE_HAS_STATUS(STEAM_BUTTON_NODE, okay)
#define STEAM_WAKE_PIN \
	NRF_GPIO_PIN_MAP(DT_PROP(DT_GPIO_CTLR(STEAM_BUTTON_NODE, gpios), port), \
			 DT_GPIO_PIN(STEAM_BUTTON_NODE, gpios))
#if (DT_GPIO_FLAGS(STEAM_BUTTON_NODE, gpios) & GPIO_ACTIVE_LOW)
#define STEAM_WAKE_SENSE NRF_GPIO_PIN_SENSE_LOW
#else
#define STEAM_WAKE_SENSE NRF_GPIO_PIN_SENSE_HIGH
#endif
#endif

LOG_MODULE_REGISTER(power);

#define POWER_OFF_CHIME_MS 420
static uint32_t boot_reset_reason;
static uint32_t boot_gpio_latches[2];
static bool boot_steam_raw; /* diagnosis (v51.5 test 5): Steam held at the very first moment */
static bool boot_state_captured;
static atomic_t power_off_claimed;
#define CRASH_MAGIC 0x5C26C0DEU
static __noinit struct
{
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	char thread[12];
} crash_record;
static struct power_crash_info last_crash;
static uint32_t power_off_release_mask;
static bool power_off_quiet_request;
K_SEM_DEFINE(power_off_sem, 0, 1);

static void power_arm_systemoff_wake(void)
{
	for(uint32_t port = 0; port < NRF_GPIO_PORT_COUNT; ++port)
	{
		for(uint32_t pin = 0; pin < NRF_GPIO_PINS_PER_PORT; ++pin)
		{
			uint32_t nrf_pin = NRF_GPIO_PIN_MAP(port, pin);

			if(nrf_gpio_pin_present_check(nrf_pin))
			{
				nrf_gpio_cfg_sense_set(nrf_pin, NRF_GPIO_PIN_NOSENSE);
			}
		}
	}

#if DT_NODE_HAS_STATUS(STEAM_BUTTON_NODE, okay)
	nrf_gpio_cfg_sense_set(STEAM_WAKE_PIN, STEAM_WAKE_SENSE);
	LOG_INF("armed Steam button as sole SYSTEMOFF GPIO wake source");
#else
	LOG_WRN("no Steam button alias; SYSTEMOFF has no GPIO wake source");
#endif
}

static void power_prepare_shutdown(void)
{
	/* keys / clicks held by the remapper go up while the radio still works */
	remap_power_off();
	k_msleep(60);
#if CONFIG_IBEX_RGBW_LED
	rgbw_led_prepare_poweroff();
#endif

	transport_radio_deactivate();
}

static void power_enter_system_off(void)
{
#if CONFIG_POWEROFF
	int err;
	bool waiting_for_external_power = false;

	power_arm_systemoff_wake();
	if(IS_ENABLED(CONFIG_IBEX_BATTERY))
	{
		for(;;)
		{
			bool external_source;
			bool puck_pilot_present = false;

			/* The original release check is stale after waiting on a charger. */
			if(waiting_for_external_power && (hardware_read_buttons() & STEAM_BUTTON_MASK) != 0U)
			{
				LOG_INF("Steam pressed while externally powered: restarting");
				power_mark_continue_wireless();
				sys_reboot(SYS_REBOOT_COLD);
			}

			external_source = transport_usb_attached() || puck_interface_active();
			if(!external_source)
			{
				err = analog_puck_pilot_present(&puck_pilot_present);
				if(err)
				{
					LOG_WRN("puck pilot read failed: %d; skipping MP2733 shipping mode", err);
					err = 0;
					break;
				}
				external_source = puck_pilot_present;
			}
			err = external_source ? -EAGAIN : battery_prepare_poweroff();
			if(err != -EAGAIN)
			{
				break;
			}
			if(!waiting_for_external_power)
			{
				LOG_INF("external power present; waiting to enter MP2733 shipping mode");
				waiting_for_external_power = true;
			}
			k_msleep(POWER_OFF_EXTERNAL_POLL_MS);
			watchdog_feed();
		}
		if(err < 0)
		{
			LOG_WRN("MP2733 shipping mode failed: %d; falling back to MCU SYSTEMOFF", err);
		}
	}
	sys_poweroff();
#else
	sys_reboot(SYS_REBOOT_COLD);
#endif
}

void power_capture_boot_state(void)
{
	if(boot_state_captured)
	{
		return;
	}

#if NRF_POWER_HAS_RESETREAS
	boot_reset_reason = nrf_power_resetreas_get(NRF_POWER);
	nrf_power_resetreas_clear(NRF_POWER, boot_reset_reason);
#endif

#if defined(NRF_GPIO_LATCH_PRESENT)
	nrf_gpio_latches_read_and_clear(0, ARRAY_SIZE(boot_gpio_latches), boot_gpio_latches);
#endif
	{
		/* the Steam button (P1.06, active low), before settings take their ~2 s; the GPIO driver
		 * configures the pin again later */
		uint32_t pin = NRF_GPIO_PIN_MAP(1, 6);

		nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLUP);
		k_busy_wait(50);
		boot_steam_raw = nrf_gpio_pin_read(pin) == 0U;
	}

	if(crash_record.magic == CRASH_MAGIC)
	{
		last_crash.valid = true;
		last_crash.reason = crash_record.reason;
		last_crash.pc = crash_record.pc;
		last_crash.lr = crash_record.lr;
		memcpy(last_crash.thread, crash_record.thread, sizeof(last_crash.thread));
		last_crash.thread[sizeof(last_crash.thread) - 1U] = '\0';
	}
	crash_record.magic = 0U;
	boot_state_captured = true;
	LOG_INF("boot reset reason=0x%08x gpio_latch=p0:0x%08x p1:0x%08x", boot_reset_reason,
	        boot_gpio_latches[0], boot_gpio_latches[1]);
}

/*
 * Crash record for the report meter: a fatal error (fault, assert, stack
 * overflow) is noted in RAM that survives the reset, then the controller
 * restarts at once instead of halting until the watchdog fires.
 */

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *name = k_thread_name_get(k_current_get());

	crash_record.magic = CRASH_MAGIC;
	crash_record.reason = reason;
	crash_record.pc = esf != NULL ? esf->basic.pc : 0U;
	crash_record.lr = esf != NULL ? esf->basic.lr : 0U;
	memset(crash_record.thread, 0, sizeof(crash_record.thread));
	if(name != NULL)
	{
		strncpy(crash_record.thread, name, sizeof(crash_record.thread) - 1U);
	}
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

const struct power_crash_info *power_last_crash(void)
{
	if(!boot_state_captured)
	{
		return NULL;
	}
	return last_crash.valid ? &last_crash : NULL;
}

uint32_t power_boot_reset_reason(void)
{
	return boot_reset_reason;
}

bool power_boot_steam_raw(void)
{
	return boot_steam_raw;
}

void power_boot_gpio_latches(uint32_t latches[2])
{
	latches[0] = boot_gpio_latches[0];
	latches[1] = boot_gpio_latches[1];
}

void power_mark_continue_wireless(void)
{
	/* Same idea and magic as OFW: only when the radio is the active link. */
	if(!transport_usb_configured())
	{
		continue_wireless_magic = CONTINUE_WIRELESS_MAGIC;
	}
}

bool power_take_continue_wireless(void)
{
	bool set = continue_wireless_magic == CONTINUE_WIRELESS_MAGIC;

	continue_wireless_magic = 0;
	return set;
}

int power_reboot_normal(void)
{
	power_mark_continue_wireless();
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

void power_off_quiet(void)
{
	power_off_quiet_request = true;
	power_off();
}

void power_off(void)
{
	power_off_after_buttons_released(STEAM_BUTTON_MASK);
}

static void power_off_thread_entry(void *unused1, void *unused2, void *unused3)
{
	uint32_t release_mask;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	k_sem_take(&power_off_sem, K_FOREVER);
	release_mask = power_off_release_mask;

	int64_t chime_until = 0;

	if(!power_off_quiet_request && haptics_play_power_sound(false) == 0)
	{
		chime_until = k_uptime_get() + POWER_OFF_CHIME_MS;
	}

	power_prepare_shutdown();

	if(release_mask != 0U)
	{
		LOG_INF("waiting for power-off buttons to release: 0x%08x", release_mask);
		while((hardware_read_buttons() & release_mask) != 0U)
		{
			hardware_wait_for_change();
			k_msleep(POWER_OFF_RELEASE_POLL_MS);
			watchdog_feed();
		}
		k_msleep(POWER_OFF_RELEASE_DEBOUNCE_MS);
	}
	while(k_uptime_get() < chime_until)
	{
		/* let the power-off chime finish before the amplifier loses power */
		k_msleep(20);
		watchdog_feed();
	}

	power_enter_system_off();
}

K_THREAD_DEFINE(power_off_thread, POWER_OFF_THREAD_STACK_SIZE, power_off_thread_entry, NULL, NULL,
                NULL, POWER_OFF_THREAD_PRIORITY, 0, 0);

void power_off_after_buttons_released(uint32_t release_mask)
{
	if(!atomic_cas(&power_off_claimed, 0, 1))
	{
		LOG_DBG("power-off already in progress");
		return;
	}

	power_off_release_mask = release_mask;
	k_sem_give(&power_off_sem);
}

int power_reboot_to_valve_isp(void)
{
	volatile uint32_t *const magic = (volatile uint32_t *)VALVE_ISP_MAGIC_BASE;

	/*
	 * OFW: hid_power_write_isp_magic() writes these magic words and then
	 * the state machine performs a normal system reboot.
	 */
	magic[0] = 0xcd595b80u;
	magic[1] = 0x93541da0u;
	magic[2] = 0x0078143cu;
	magic[3] = 0x00000000u;

	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
