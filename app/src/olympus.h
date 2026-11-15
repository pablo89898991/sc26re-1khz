/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include "controller.h"

#define OLYMPUS_DESCRIPTOR_SIZE 30
#define OLYMPUS_MAX_PAYLOAD_SIZE 200

struct olympus_pad_debug
{
	int16_t x;
	int16_t y;
	uint16_t pressure;
	int16_t raw_pressure;
	int16_t raw_click;
	int16_t raw_grip;
	int32_t grip_value;
	int32_t peak_x;
	int32_t peak_y;
	int32_t peak;
	int32_t noise_threshold;
	int32_t touch_threshold;
	int32_t release_threshold;
	int32_t grip_touch_threshold;
	int32_t grip_release_threshold;
	int32_t pad_click_threshold;
	bool grip_touched;
	bool stick_touched;
	bool touched;
	bool clicked;
};

struct olympus_debug_snapshot
{
	uint32_t frame_count;
	bool ready;
	struct olympus_pad_debug left;
	struct olympus_pad_debug right;
};

int olympus_init(void);

/*
 * User capacitive thresholds (report meter, "sc26re/capsense"): stick touch on,
 * stick touch off, grip touch on, as raw sensor values. 0 = firmware default.
 * get returns the effective values; set applies live; save persists them.
 */
#define OLYMPUS_CAPSENSE_SIZE 6
void olympus_capsense_get(uint8_t out[OLYMPUS_CAPSENSE_SIZE]);
int olympus_capsense_set(const uint8_t *data, size_t len);
int olympus_capsense_save(void);
void olympus_read_report(struct controller_report *report);
void olympus_get_debug_snapshot(struct olympus_debug_snapshot *snapshot);
int olympus_read_descriptor(uint8_t descriptor[OLYMPUS_DESCRIPTOR_SIZE]);
int olympus_debug_read(uint32_t address, uint8_t *data, uint8_t size, bool raw);
