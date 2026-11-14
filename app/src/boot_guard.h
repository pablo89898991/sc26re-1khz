/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Startup safeguard.
 *  - Keeps the bootloader-armed watchdog (5 s) fed while main() initialises, so a slow
 *    first boot after flashing (settings migration, USB enumeration) cannot reset-loop.
 *  - Counts boots that never reached a stable main loop (retained RAM, survives resets).
 *  - Init failures no longer return from main() (which starved the watchdog and looped
 *    the controller with the boot LED blinking): retry with a reboot, then give up cleanly.
 */
void boot_guard_begin(void);
/* boots in a row that did not stay up for BOOT_GUARD_STABLE_MS, including this one */
uint8_t boot_guard_attempts(void);
bool boot_guard_safe_mode(void);
/* call every main-loop iteration */
void boot_guard_loop(void);
/* a required init step failed: reboot to retry, or power off after too many tries */
void boot_guard_fatal(const char *stage, int err);
