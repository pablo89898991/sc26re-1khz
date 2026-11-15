/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct controller_report;

int imu_init(void);
int imu_read_report(struct controller_report *report);
bool imu_ready(void);

int imu_calibrate_gyro(void);
/* 0 idle, 1 measuring, 2 saved, 3 moved (kept old), 4 save failed */
uint8_t imu_gyro_cal_state(void);
void imu_reset(void);

bool imu_settings_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);
int imu_settings_stage(const char *path, const uint8_t *value, size_t len);
int imu_settings_commit(const char *path);

void imu_debug_note_scan_us(uint32_t us);
