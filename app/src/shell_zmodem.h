/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <zephyr/shell/shell.h>

#include "zmodem.h"

/* Call from a shell command handler; the command owns the transport until done. */
int shell_zmodem_send(const struct shell *shell, const struct zmodem_file *file);
