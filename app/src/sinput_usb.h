/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "controller.h"

/*
 * Experimental SInput HID protocol (Hand Held Legend, SDL3 "SInput" driver)
 * for the USB link only. Wire format follows SINPUT-LIB-HID (MIT-0).
 */
#define SINPUT_REPORT_SIZE 64U
#define SINPUT_REPORT_ID_INPUT 0x01U
#define SINPUT_REPORT_ID_INPUT_CMDDAT 0x02U
#define SINPUT_REPORT_ID_OUTPUT_CMDDAT 0x03U

extern const uint8_t sinput_hid_report_map[];
extern const size_t sinput_hid_report_map_size;

/* Build report 0x01. out[0] is the report ID; returns the full length (64). */
size_t sinput_build_input_report(uint8_t out[SINPUT_REPORT_SIZE], const struct controller_report *report);

/* Remember the latest battery state for the plug/charge bytes. */
void sinput_note_battery(const struct controller_battery_report *report);

/*
 * Handle an output report. data may start with the 0x03 report ID or not
 * (report_id tells which). If a reply is due (features), it is written to
 * reply (64 bytes incl. report ID) and its length is returned; otherwise 0.
 */
size_t sinput_handle_output(uint8_t report_id, const uint8_t *data, size_t len,
                            uint8_t reply[SINPUT_REPORT_SIZE]);
