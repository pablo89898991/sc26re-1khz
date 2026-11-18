/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdint.h>

struct controller_report;

/* "r<0.01 %>" ramp, "h<0.01 %>" hold, "x" cancel */
int stick_test_command(const char *arg);
/* 0 state (0 idle, 1 armed, 2 running, 3 done, 4 cancelled), 1 kind, 2 laps, 4 ramp result u16,
 * 6 deflection u16, 8.. lap times u32 x4 (ms since the start) */
void stick_test_state(uint8_t out[24]);
/* raw = buttons before the remapper */
void stick_test_apply(struct controller_report *r, uint32_t raw);
