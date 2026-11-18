/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define VALVE_HID_REPORT_MAP_SIZE 372
#define VALVE_HID_REPORT_MAP_USB_SIZE 425

extern const uint8_t valve_hid_report_map[VALVE_HID_REPORT_MAP_SIZE];
extern const size_t valve_hid_report_map_size;
extern const uint8_t valve_hid_report_map_usb[VALVE_HID_REPORT_MAP_USB_SIZE];

void valve_hid_report_map_copy_ble(uint8_t *dst, size_t dst_size);
