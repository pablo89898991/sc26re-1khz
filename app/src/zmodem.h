/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct zmodem_io
{
	void *context;
	/* Return a byte, or a negative errno (including -ETIMEDOUT). */
	int (*receive)(void *context, uint32_t timeout_ms);
	/* Write all bytes, or return a negative errno. */
	int (*send)(void *context, const uint8_t *data, size_t size);
	int64_t (*now)(void *context);
};

struct zmodem_file
{
	const char *name;
	uint32_t size;
	void *context;
	/* Fill the requested range, or return a negative errno. May be called again on retry. */
	int (*read)(void *context, uint32_t offset, uint8_t *data, size_t size);
};

/* Send one binary file. Returns 0, -EEXIST if skipped, or another negative errno.
 * Uses CRC-16, bounded packets with acknowledgments, and the standard hex replies.
 * The transport must be exclusively owned by the caller until this returns.
 */
int zmodem_send(const struct zmodem_io *io, const struct zmodem_file *file);
