/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stdbool.h>

struct controller_report;
struct usbd_context;

/* before the classes are registered: adds the MS OS 2.0 descriptor when on */
void xinput_setup(struct usbd_context *ctx, bool on);
const char *xinput_class_name(void);
/* XInput interface on and configured by the host */
bool xinput_enabled(void);
/* send the final pad state as an Xbox 360 report (only when it changed) */
void xinput_send(const struct controller_report *r);
/* XInput active: leave only Steam / QAM in the Steam pad report */
void xinput_neutral_pad(struct controller_report *r);
