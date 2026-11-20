/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct controller_report;
struct usbd_context;

/* before the classes are registered: adds the MS OS 2.0 descriptor when on */
void xinput_setup(struct usbd_context *ctx, bool on);
const char *xinput_class_name(void);
/* XInput interface on and configured by the host */
bool xinput_enabled(void);
/* ... and a driver reads it: a report waiting longer than 250 ms means nothing reads the Xbox
 * pad (no driver bound): its outputs then go to the Steam pad, the Steam pad stays whole */
bool xinput_live(void);
/* diagnostics: [0] bit0 on at power-on, bit1 interface enabled by the host, bit2 class bound;
 * [1] pad_out now; [2..3] reports the host took; [4..5] vendor requests seen */
void xinput_diag(uint8_t out[8]); /* flags, pad_out, reports taken, class requests, MS OS 2.0 reads */
/* send the final pad state as an Xbox 360 report (only when it changed) */
void xinput_send(const struct controller_report *r, uint32_t extra);
/* XInput active: leave only Steam / QAM in the Steam pad report */
void xinput_neutral_pad(struct controller_report *r);

/* touch sensors do not make a Steam press a chord */
#define XI_GUIDE_IGNORE (BIT(20) | BIT(21) | BIT(24) | BIT(25) | BIT(28) | BIT(29))
/* option xi_guide: true while the Xbox Guide tap is on */
bool xinput_guide_tap(uint32_t buttons, int64_t now);
/* the Steam button to Steam, held back while it is held alone (see xinput_guide.c) */
uint32_t steam_button_hold_back(uint32_t out, uint32_t phys, int64_t now);
