/* SPDX-License-Identifier: AGPL-3.0-or-later */

#pragma once

#include <stdbool.h>

int puck_interface_init(void);
bool puck_interface_active(void);
/* a valid Puck frame arrived within the last ms (stray bytes on open contacts don't count) */
bool puck_interface_frame_within(uint32_t ms);
void puck_interface_counters(uint32_t *frames, uint32_t *rx_bytes, uint32_t *frame_age_ms);
