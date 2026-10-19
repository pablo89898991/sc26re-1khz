/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "zmodem.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ZMODEM protocol, Oct 1988: https://pauillac.inria.fr/~doligez/zmodem/zmodem.txt
 * Hex headers may precede CRC-16 data. ZCRCW ends each frame and requests an
 * ACK, bounding both receiver buffering and the amount retransmitted on error.
 */
#define ZPAD '*'
#define ZDLE 0x18
#define ZHEX 'B'
#define ZCRCW 'k'
#define ZRQINIT 0
#define ZRINIT 1
#define ZACK 3
#define ZFILE 4
#define ZSKIP 5
#define ZABORT 7
#define ZFIN 8
#define ZRPOS 9
#define ZDATA 10
#define ZEOF 11
#define ZFERR 12
#define ZCRC 13
#define ZCHALLENGE 14
#define ZCAN 16
#define PACKET_SIZE 256
#define TIMEOUT_MS 10000
#define MAX_RETRIES 5

struct session
{
	const struct zmodem_io *io;
	int64_t deadline;
	unsigned int cancel_count;
};

static uint16_t crc16_byte(uint16_t crc, uint8_t byte)
{
	crc ^= (uint16_t)byte << 8;
	for(int bit = 0; bit < 8; ++bit)
	{
		crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
	}
	return crc;
}

static int send_header(const struct zmodem_io *io, uint8_t type, uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	uint8_t header[7] = { type, value, value >> 8, value >> 16, value >> 24 };
	uint8_t wire[21] = { ZPAD, ZPAD, ZDLE, ZHEX };
	uint16_t crc = 0;
	size_t size = 4;

	for(size_t i = 0; i < 5; ++i)
	{
		crc = crc16_byte(crc, header[i]);
	}
	header[5] = crc >> 8;
	header[6] = crc;
	for(size_t i = 0; i < sizeof(header); ++i)
	{
		wire[size++] = digits[header[i] >> 4];
		wire[size++] = digits[header[i] & 15];
	}
	wire[size++] = '\r';
	wire[size++] = '\n';
	if(type != ZACK && type != ZFIN)
	{
		wire[size++] = 0x11;
	}
	return io->send(io->context, wire, size);
}

static int send_packet(const struct zmodem_io *io, const uint8_t *data, size_t size)
{
	uint8_t wire[64];
	uint16_t crc = 0;
	size_t used = 0;

	for(size_t i = 0; i < size; ++i)
	{
		crc = crc16_byte(crc, data[i]);
	}
	crc = crc16_byte(crc, ZCRCW);
	for(size_t i = 0; i < size + 4; ++i)
	{
		uint8_t byte;

		if(i == size)
		{
			wire[used++] = ZDLE;
			wire[used++] = ZCRCW;
			continue;
		}
		if(i == size + 3)
		{
			wire[used++] = 0x11;
		}
		else
		{
			byte = i < size ? data[i] : (i == size + 1 ? crc >> 8 : crc);
			/* Escape all control characters, including their high-bit variants. */
			if((byte & 0x60) == 0 || byte == 0x7f || byte == 0xff)
			{
				wire[used++] = ZDLE;
				byte = byte == 0x7f ? 'l' : byte == 0xff ? 'm' : byte ^ 0x40;
			}
			wire[used++] = byte;
		}
		if(used >= sizeof(wire) - 4 || i == size + 3)
		{
			int err = io->send(io->context, wire, used);

			if(err)
			{
				return err;
			}
			used = 0;
		}
	}
	return 0;
}

static int receive_byte(struct session *session)
{
	int64_t remaining = session->deadline - session->io->now(session->io->context);
	int byte;

	if(remaining <= 0)
	{
		return -ETIMEDOUT;
	}
	byte = session->io->receive(session->io->context, (uint32_t)remaining);
	if(byte < 0)
	{
		return byte;
	}
	byte &= 0x7f;
	session->cancel_count = byte == ZDLE ? session->cancel_count + 1 : 0;
	return byte == 3 || session->cancel_count >= 5 ? -ECANCELED : byte;
}

static int receive_hex(struct session *session)
{
	int value = 0;

	for(int i = 0; i < 2; ++i)
	{
		int digit = receive_byte(session);

		if(digit < 0)
		{
			return digit;
		}
		if(digit >= '0' && digit <= '9')
		{
			digit -= '0';
		}
		else if(digit >= 'a' && digit <= 'f')
		{
			digit -= 'a' - 10;
		}
		else
		{
			return -EBADMSG;
		}
		value = (value << 4) | digit;
	}
	return value;
}

static int receive_header(struct session *session, uint32_t *value)
{
	uint8_t header[7];
	uint16_t crc = 0;
	unsigned int prefix = 0;

	session->deadline = session->io->now(session->io->context) + TIMEOUT_MS;
	while(prefix != 3)
	{
		int byte = receive_byte(session);

		if(byte < 0)
		{
			return byte;
		}
		if(byte == ZPAD)
		{
			prefix = 1;
		}
		else if(prefix == 1 && byte == ZDLE)
		{
			prefix = 2;
		}
		else if(prefix == 2 && byte == ZHEX)
		{
			prefix = 3;
		}
		else
		{
			prefix = 0;
		}
	}
	for(size_t i = 0; i < sizeof(header); ++i)
	{
		int byte = receive_hex(session);

		if(byte < 0)
		{
			return byte;
		}
		header[i] = byte;
		crc = crc16_byte(crc, byte);
	}
	if(crc != 0)
	{
		return -EBADMSG;
	}
	/* Consume the line ending, including ZFIN's, before returning to a shell. */
	int end = receive_byte(session);
	if(end == '\r')
	{
		end = receive_byte(session);
	}
	if(end != '\n')
	{
		return end < 0 ? end : -EBADMSG;
	}
	*value = (uint32_t)header[1] |
	         (uint32_t)header[2] << 8 |
	         (uint32_t)header[3] << 16 |
	         (uint32_t)header[4] << 24;
	return header[0];
}

static int send_file_crc(const struct zmodem_io *io, const struct zmodem_file *file, uint32_t size,
                         uint8_t buffer[PACKET_SIZE])
{
	uint32_t crc = UINT32_MAX;

	if(size == 0)
	{
		size = file->size;
	}
	if(size > file->size)
	{
		return -EINVAL;
	}
	for(uint32_t offset = 0; offset < size;)
	{
		size_t chunk = size - offset < PACKET_SIZE ? size - offset : PACKET_SIZE;
		int err = file->read(file->context, offset, buffer, chunk);

		if(err)
		{
			return err;
		}
		for(size_t i = 0; i < chunk; ++i)
		{
			crc ^= buffer[i];
			for(int bit = 0; bit < 8; ++bit)
			{
				crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
			}
		}
		offset += chunk;
	}
	return send_header(io, ZCRC, ~crc);
}

int zmodem_send(const struct zmodem_io *io, const struct zmodem_file *file)
{
	enum
	{
		INIT,
		FILE_INFO,
		DATA,
		END,
		FINISH
	} state = INIT;
	struct session session = { .io = io };
	uint8_t buffer[PACKET_SIZE];
	uint32_t offset = 0;
	uint32_t high_water = 0;
	size_t packet_size = sizeof(buffer);
	size_t chunk = 0;
	unsigned int retries = 0;
	bool send = true;
	int outcome = 0;
	int err = 0;

	if(!file->name[0] ||
	   strlen(file->name) > 128 ||
	   strchr(file->name, '/') ||
	   strchr(file->name, '\\'))
	{
		return -EINVAL;
	}
	for(;;)
	{
		uint32_t value = 0;
		int type;

		if(send)
		{
			switch(state)
			{
				case INIT:
					err = send_header(io, ZRQINIT, 0);
					break;
				case FILE_INFO:
				{
					size_t name_size = strlen(file->name) + 1;
					memcpy(buffer, file->name, name_size);
					int info_size = snprintf((char *)&buffer[name_size], sizeof(buffer) - name_size,
					                         "%" PRIu32, file->size);
					/* ZBIN conversion, default receiver file-management policy. */
					err = send_header(io, ZFILE, 1U << 24);
					if(!err)
					{
						err = send_packet(io, buffer, name_size + info_size + 1);
					}
					break;
				}
				case DATA:
					chunk = file->size - offset < packet_size ? file->size - offset : packet_size;
					err = file->read(file->context, offset, buffer, chunk);
					if(!err)
					{
						err = send_header(io, ZDATA, offset);
					}
					if(!err)
					{
						err = send_packet(io, buffer, chunk);
					}
					break;
				case END:
					err = send_header(io, ZEOF, offset);
					break;
				case FINISH:
					err = send_header(io, ZFIN, 0);
					break;
			}
			if(err)
			{
				break;
			}
		}
		send = true;
		type = receive_header(&session, &value);
		if(type == -ECANCELED || type == ZCAN || type == ZABORT || type == ZFERR)
		{
			err = -ECANCELED;
			break;
		}
		if(type < 0 && type != -ETIMEDOUT && type != -EBADMSG)
		{
			err = type;
			break;
		}
		if(type == ZRINIT && state == INIT)
		{
			uint16_t receive_size = value;
			if(receive_size && receive_size < packet_size)
			{
				packet_size = receive_size;
			}
			state = FILE_INFO;
			retries = 0;
			continue;
		}
		if(type == ZRINIT && state == END)
		{
			state = FINISH;
			retries = 0;
			continue;
		}
		if(type == ZFIN && state == FINISH)
		{
			err = io->send(io->context, (const uint8_t *)"OO", 2);
			return err ? err : outcome;
		}
		if(type == ZSKIP && state == FILE_INFO)
		{
			outcome = -EEXIST;
			state = FINISH;
			retries = 0;
			continue;
		}
		if(type == ZACK && state == DATA && value == offset + chunk)
		{
			offset = value;
			if(offset > high_water)
			{
				high_water = offset;
				retries = 0;
			}
			state = offset == file->size ? END : DATA;
			continue;
		}
		if((type == ZRINIT && (state == FILE_INFO || state == FINISH)) ||
		   (type == ZACK && (state == DATA || state == END) && value <= offset))
		{
			/* Replies can already be in flight when we advance to the next
			 * frame. Wait for its reply instead of multiplying duplicates.
			 */
			send = false;
		}
		else if(type == ZRPOS && (state == FILE_INFO || state == DATA || state == END))
		{
			if(value > file->size)
			{
				err = -EINVAL;
				break;
			}
			bool initial = state == FILE_INFO;
			offset = value;
			state = offset == file->size ? END : DATA;
			if(initial)
			{
				high_water = offset;
				retries = 0;
				continue;
			}
		}
		else if(type == ZCRC && state == FILE_INFO)
		{
			err = send_file_crc(io, file, value, buffer);
			send = false;
		}
		else if(type == ZCHALLENGE)
		{
			err = send_header(io, ZACK, value);
			send = false;
		}
		if(err)
		{
			break;
		}
		if(++retries >= MAX_RETRIES)
		{
			err = type == -ETIMEDOUT ? -ETIMEDOUT : -EPROTO;
			break;
		}
	}
	/* Cancel the peer before restoring ordinary shell output. */
	static const uint8_t cancel[] = { ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, ZDLE, 8,
	                                  8,    8,    8,    8,    8,    8,    8,    8,    8 };
	(void)io->send(io->context, cancel, sizeof(cancel));
	return err;
}
