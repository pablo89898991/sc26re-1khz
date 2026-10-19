/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell_log_backend.h>

#include "shell_zmodem.h"

static int64_t transfer_now(void *context)
{
	ARG_UNUSED(context);
	return k_uptime_get();
}

static int transfer_receive(void *context, uint32_t timeout_ms)
{
	const struct shell *shell = context;
	int64_t deadline = k_uptime_get() + timeout_ms;

	do
	{
		uint8_t byte;
		size_t count;
		int err = shell->iface->api->read(shell->iface, &byte, 1, &count);

		if(err)
		{
			return err;
		}
		if(count)
		{
			return byte;
		}
		k_sleep(K_MSEC(1));
	} while(k_uptime_get() < deadline);
	return -ETIMEDOUT;
}

static int transfer_send(void *context, const uint8_t *data, size_t size)
{
	const struct shell *shell = context;
	int64_t deadline = k_uptime_get() + 5000;

	while(size)
	{
		size_t count;
		int err = shell->iface->api->write(shell->iface, data, size, &count);

		if(err)
		{
			return err;
		}
		data += count;
		size -= count;
		if(k_uptime_get() >= deadline)
		{
			return -ETIMEDOUT;
		}
		if(!count)
		{
			k_sleep(K_MSEC(1));
		}
	}
	return 0;
}

int shell_zmodem_send(const struct shell *shell, const struct zmodem_file *file)
{
	const struct zmodem_io io = {
		.context = (void *)shell,
		.receive = transfer_receive,
		.send = transfer_send,
		.now = transfer_now,
	};
	bool logs_enabled =
	    IS_ENABLED(CONFIG_SHELL_LOG_BACKEND) && log_backend_is_active(shell->log_backend->backend);
	int err;

	/* The shell parser runs on this thread, so the command can consume raw RX.
	 * Hold its output lock and suspend this backend's logs during the transfer.
	 */
	k_sem_take(&shell->ctx->lock_sem, K_FOREVER);
	if(logs_enabled)
	{
		log_backend_deactivate(shell->log_backend->backend);
	}
	z_shell_fprintf_buffer_flush(shell->fprintf_ctx);
	err = zmodem_send(&io, file);
	if(logs_enabled)
	{
		/* Reactivate without resetting the user's per-module log filters. */
		log_backend_activate(shell->log_backend->backend, (void *)shell);
	}
	k_sem_give(&shell->ctx->lock_sem);
	return err;
}
