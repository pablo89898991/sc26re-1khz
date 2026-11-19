/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Hardware test of the remapper (test builds only, CONFIG_IBEX_HWTEST).
 *
 * The test page loads a short script of input frames into RAM and starts it.
 * For the script's length the report loop replaces the controller's own
 * inputs (buttons, triggers, sticks, trackpads; motion zeroed) with the
 * script's, and records every change of the remapper's output (the same
 * values as the page's output monitor) with its time. The page reads the
 * record back and the simulator, built from the same remap.c, plays the same
 * script: any difference is a bug.
 *
 * RAM only, nothing is stored. It uses the remapper's profile staging buffer
 * (free between profile uploads; an upload ends the test). A script runs at
 * most 10 s and stops at once when the physical Steam button is pressed.
 */
#ifndef HWTEST_H_
#define HWTEST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

#define HWT_STEP_SIZE 30U /* u16 ms, u32 buttons, i16 trig[2], i16 sticks[4], pads lx ly lp rx ry rp */
#define HWT_MAX_STEPS 40U
#define HWT_ENTRY_SIZE 30U /* u16 ms, mouse buttons, shift groups, kbd[8], pad u32, xbox u32, dx i32, dy i32, wheel i16 */
#define HWT_MAX_MS 10000U
#define HWT_MOVE_MS 50     /* mouse / wheel movement alone: logged at most this often */

/* report loop: before anything reads the inputs / after the remapper and the Xbox output */
void hwtest_apply(struct controller_report *r, int64_t now_ms);
void hwtest_log(int64_t now_ms);
bool hwtest_running(void);

/* feature settings: SET sc26re/hwt, GET sc26re/hwt_st and sc26re/hwt_log */
int hwtest_set(const uint8_t *v, size_t len);
bool hwtest_read(const char *path, uint8_t *buf, size_t capacity, size_t *len);

/* remap.c: its profile staging buffer while no upload uses it (NULL when busy); any upload
 * ends the test */
uint8_t *remap_hwtest_buffer(size_t *size);
void remap_hwtest_release(void);
void hwtest_buffer_lost(void);

#endif
