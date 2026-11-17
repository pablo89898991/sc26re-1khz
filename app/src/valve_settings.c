/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "calibration.h"
#include "ibex_settings_registry.h"
#include "imu.h"
#include "gyro_mouse.h"
#include "remap.h"
#include "valve_nvs.h"
#include "olympus.h"
#include "sc26re_cfg.h"
#include "valve_feature.h"
#include "valve_settings.h"
#include "power.h"

/* Report meter: stick/grip capacitive thresholds (live via STAGE, saved via COMMIT). */
#define SC26RE_CAPSENSE_PATH "sc26re/capsense"
#include "haptics.h"

LOG_MODULE_REGISTER(valve_settings);

#define VALVE_ESB_SLOT_0_TRANSPORT 2
#define VALVE_ESB_SLOT_1_TRANSPORT 3
#define VALVE_NVS_CHUNK_SIZE 48
#define VALVE_NVS_INFO_SIZE 8
#define VALVE_WIRELESS_TRANSPORT_PATH "user/wireless_transport"
/* OFW 6AE90BC5: power on/off chime level, 0 = off, 1 = low (-18 dB), 2 = high (-12 dB, default) */
#define VALVE_HAPTIC_BOOT_LEVEL_PATH "user/haptic_boot_level"
#define VALVE_HAPTIC_BOOT_LEVEL_DEFAULT 2U

static const char *const esb_bond_paths[VALVE_ESB_BOND_SLOT_COUNT] = {
	"esb/bond",
	"esb/bond_2",
};

struct esb_bond
{
	uint8_t data[VALVE_ESB_BOND_SIZE];
	bool valid;
};

struct esb_bond_slot
{
	struct esb_bond staged;
	struct esb_bond active;
	bool dirty;
};

static struct esb_bond_slot esb_bonds[VALVE_ESB_BOND_SLOT_COUNT];
static uint8_t wireless_transport;
static bool wireless_transport_valid;
static bool wireless_transport_dirty;
static uint8_t haptic_boot_level = VALVE_HAPTIC_BOOT_LEVEL_DEFAULT;
static bool haptic_boot_level_valid;
K_MUTEX_DEFINE(esb_settings_mutex);
K_MUTEX_DEFINE(valve_settings_commit_mutex);
static struct k_spinlock active_esb_bond_lock;
static uint8_t active_wireless_transport;
static bool active_wireless_transport_valid;
static valve_settings_esb_bond_changed_cb_t esb_bond_changed_callback;

struct feature_state_load_target
{
	void *value;
	size_t value_size;
	bool *valid;
};

static int load_feature_state_cb(const char *key, size_t len, settings_read_cb read_cb,
                                 void *cb_arg, void *param)
{
	struct feature_state_load_target *target = param;
	ssize_t read_len;

	if(key != NULL && key[0] != '\0')
	{
		return 0;
	}
	if(len != target->value_size)
	{
		return -EINVAL;
	}
	read_len = read_cb(cb_arg, target->value, target->value_size);
	if(read_len == (ssize_t)target->value_size)
	{
		*target->valid = true;
	}
	return 1;
}

static int esb_bond_slot(const char *path)
{
	for(size_t slot = 0; slot < ARRAY_SIZE(esb_bond_paths); ++slot)
	{
		if(strcmp(path, esb_bond_paths[slot]) == 0)
		{
			return (int)slot;
		}
	}
	return -1;
}

static int esb_bond_slot_for_transport(uint8_t transport)
{
	switch(transport)
	{
		case VALVE_ESB_SLOT_0_TRANSPORT:
			return 0;
		case VALVE_ESB_SLOT_1_TRANSPORT:
			return 1;
		default:
			return -1;
	}
}

static bool esb_bond_material_valid(const uint8_t *bond)
{
	/* The original transport considers a slot stored only when both UUID words are nonzero. */
	return sys_get_le32(&bond[0]) != 0U && sys_get_le32(&bond[4]) != 0U;
}

static bool esb_bond_available(size_t slot)
{
	const struct esb_bond *bond = &esb_bonds[slot].staged;

	return bond->valid && esb_bond_material_valid(bond->data);
}

static bool active_esb_bond_available_locked(size_t slot)
{
	const struct esb_bond *bond = &esb_bonds[slot].active;

	return bond->valid && esb_bond_material_valid(bond->data);
}

static int ofw_storage_read(const void *context, uint32_t offset, void *data, size_t len)
{
	return flash_area_read(context, offset, data, len);
}

/*
 * The Valve bootloader erases the CFW settings area on every flash, so a Puck bond
 * made under the original firmware would be lost. Fall back (read-only) to the bonds
 * and wireless transport stored by the original firmware in valve_storage.
 */
/*
 * Slots the user forgot (Steam "forget" or hard reset), bit per slot. The stock
 * bond in Valve's storage is not imported again for these. Kept separately so
 * the forgotten slot itself is a plain missing setting (v40 stored an all-zero
 * "tombstone" there, which Steam read as a bond to an unknown Puck).
 */
static int64_t meter_awake_ms = -100000;

bool valve_settings_meter_keeps_awake(void)
{
	return k_uptime_get() - meter_awake_ms < 6000;
}

#define VALVE_ESB_FORGOT_PATH "user/esb_forgot"
static uint8_t esb_forgot_mask;

static void esb_forgot_save_locked(void)
{
	if(IS_ENABLED(CONFIG_SETTINGS))
	{
		(void)settings_save_one(VALVE_ESB_FORGOT_PATH, &esb_forgot_mask, sizeof(esb_forgot_mask));
	}
}

static void import_ofw_esb_bonds_locked(void)
{
	const struct flash_area *fa;
	struct valve_nvs nvs;
	bool imported = false;

	if(IS_ENABLED(CONFIG_IBEX_ESB_IMPORT_OFW_BONDS) == false)
	{
		return;
	}
	if(flash_area_open(PARTITION_ID(valve_storage), &fa) != 0)
	{
		return;
	}
	if(valve_nvs_open(&nvs, ofw_storage_read, fa, fa->fa_size) != 0)
	{
		flash_area_close(fa);
		return;
	}
	for(size_t slot = 0; slot < ARRAY_SIZE(esb_bonds); ++slot)
	{
		struct esb_bond *bond = &esb_bonds[slot].staged;
		uint8_t data[VALVE_ESB_BOND_SIZE];

		/* a stored bond wins; a forgotten slot must not get the stock bond back */
		if(bond->valid || (esb_forgot_mask & BIT(slot)))
		{
			continue;
		}
		int len = valve_nvs_read_setting(&nvs, esb_bond_paths[slot], data, sizeof(data));

		LOG_INF("OFW %s: %d", esb_bond_paths[slot], len);
		if(len >= (int)sizeof(data) && esb_bond_material_valid(data))
		{
			memcpy(bond->data, data, sizeof(data));
			bond->valid = true;
			imported = true;
			LOG_INF("imported OFW ESB bond slot %u", (unsigned int)slot);
		}
	}
	if(!wireless_transport_valid || imported)
	{
		uint8_t transport;

		if(valve_nvs_read_setting(&nvs, VALVE_WIRELESS_TRANSPORT_PATH, &transport,
		                          sizeof(transport)) == (int)sizeof(transport) &&
		   !wireless_transport_valid && esb_bond_slot_for_transport(transport) >= 0)
		{
			wireless_transport = transport;
			wireless_transport_valid = true;
			LOG_INF("imported OFW wireless transport %u", transport);
		}
	}
	if(!haptic_boot_level_valid)
	{
		uint8_t level;

		if(valve_nvs_read_setting(&nvs, VALVE_HAPTIC_BOOT_LEVEL_PATH, &level, sizeof(level)) ==
		   (int)sizeof(level))
		{
			haptic_boot_level = MIN(level, 2U);
			haptic_boot_level_valid = true;
			LOG_INF("imported OFW haptic boot level %u", level);
		}
	}
	flash_area_close(fa);
}

static void select_available_wireless_transport(void)
{
	int slot;

	/* Original firmware uses zero for Bluetooth, which intentionally selects no ESB slot. */
	if(wireless_transport_valid && wireless_transport == 0)
	{
		return;
	}

	slot = esb_bond_slot_for_transport(wireless_transport);
	if(wireless_transport_valid && slot >= 0 && esb_bond_available(slot))
	{
		return;
	}

	if(wireless_transport_valid)
	{
		LOG_WRN("wireless transport %u does not select an available ESB bond", wireless_transport);
	}
	wireless_transport_valid = false;

	for(size_t candidate = 0; candidate < ARRAY_SIZE(esb_bonds); ++candidate)
	{
		if(esb_bond_available(candidate))
		{
			wireless_transport =
			    candidate == 0 ? VALVE_ESB_SLOT_0_TRANSPORT : VALVE_ESB_SLOT_1_TRANSPORT;
			wireless_transport_valid = true;
			return;
		}
	}
}

static void notify_esb_bond_changed(void)
{
	if(esb_bond_changed_callback != NULL)
	{
		esb_bond_changed_callback();
	}
}

void valve_settings_load_feature_state(void)
{
	struct feature_state_load_target transport_target = {
		.value = &wireless_transport,
		.value_size = sizeof(wireless_transport),
		.valid = &wireless_transport_valid,
	};
	k_spinlock_key_t key;

	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		return;
	}

	k_mutex_lock(&esb_settings_mutex, K_FOREVER);
	{
		bool forgot_valid = false;
		struct feature_state_load_target forgot_target = {
			.value = &esb_forgot_mask,
			.value_size = sizeof(esb_forgot_mask),
			.valid = &forgot_valid,
		};

		esb_forgot_mask = 0;
		(void)settings_load_subtree_direct(VALVE_ESB_FORGOT_PATH, load_feature_state_cb,
		                                   &forgot_target);
		if(!forgot_valid)
		{
			esb_forgot_mask = 0;
		}
	}
	for(size_t slot = 0; slot < ARRAY_SIZE(esb_bonds); ++slot)
	{
		struct esb_bond_slot *bond_slot = &esb_bonds[slot];
		struct feature_state_load_target target = {
			.value = bond_slot->staged.data,
			.value_size = sizeof(bond_slot->staged.data),
			.valid = &bond_slot->staged.valid,
		};

		bond_slot->staged.valid = false;
		(void)settings_load_subtree_direct(esb_bond_paths[slot], load_feature_state_cb, &target);
		bond_slot->dirty = false;
		if(bond_slot->staged.valid && !esb_bond_material_valid(bond_slot->staged.data))
		{
			/* v40/v41 tombstone: turn it into a missing setting plus a forgot bit */
			LOG_INF("ESB bond slot %u was forgotten; clearing the old empty entry",
			        (unsigned int)slot);
			memset(&bond_slot->staged, 0, sizeof(bond_slot->staged));
			esb_forgot_mask |= BIT(slot);
			if(IS_ENABLED(CONFIG_SETTINGS))
			{
				(void)settings_delete(esb_bond_paths[slot]);
			}
			esb_forgot_save_locked();
		}
	}

	wireless_transport_valid = false;
	(void)settings_load_subtree_direct(VALVE_WIRELESS_TRANSPORT_PATH, load_feature_state_cb,
	                                   &transport_target);
	{
		struct feature_state_load_target boot_level_target = {
			.value = &haptic_boot_level,
			.value_size = sizeof(haptic_boot_level),
			.valid = &haptic_boot_level_valid,
		};

		(void)settings_load_subtree_direct(VALVE_HAPTIC_BOOT_LEVEL_PATH, load_feature_state_cb,
		                                   &boot_level_target);
		haptic_boot_level = MIN(haptic_boot_level, 2U);
	}
	import_ofw_esb_bonds_locked();
	select_available_wireless_transport();
	wireless_transport_dirty = false;
	key = k_spin_lock(&active_esb_bond_lock);
	for(size_t slot = 0; slot < ARRAY_SIZE(esb_bonds); ++slot)
	{
		esb_bonds[slot].active = esb_bonds[slot].staged;
	}
	active_wireless_transport = wireless_transport;
	active_wireless_transport_valid = wireless_transport_valid;
	k_spin_unlock(&active_esb_bond_lock, key);
	k_mutex_unlock(&esb_settings_mutex);
	notify_esb_bond_changed();
}

bool valve_settings_copy_active_esb_bond(uint8_t *bond, size_t capacity, uint8_t *slot)
{
	k_spinlock_key_t key;
	int active_slot;
	bool available;

	if(bond == NULL || capacity < VALVE_ESB_BOND_SIZE)
	{
		return false;
	}

	key = k_spin_lock(&active_esb_bond_lock);
	if(!active_wireless_transport_valid)
	{
		k_spin_unlock(&active_esb_bond_lock, key);
		return false;
	}
	active_slot = esb_bond_slot_for_transport(active_wireless_transport);
	available = active_slot >= 0 && active_esb_bond_available_locked(active_slot);
	if(available)
	{
		memcpy(bond, esb_bonds[active_slot].active.data, VALVE_ESB_BOND_SIZE);
	}
	k_spin_unlock(&active_esb_bond_lock, key);
	if(!available)
	{
		return false;
	}
	if(slot != NULL)
	{
		*slot = (uint8_t)active_slot;
	}
	return true;
}

int valve_settings_register_esb_bond_changed_callback(valve_settings_esb_bond_changed_cb_t callback)
{
	if(callback == NULL)
	{
		return -EINVAL;
	}
	if(esb_bond_changed_callback != NULL && esb_bond_changed_callback != callback)
	{
		return -EALREADY;
	}

	esb_bond_changed_callback = callback;
	callback();
	return 0;
}

static bool calibration_path_side(const char *path, enum calibration_side *side)
{
	if(strcmp(path, "cal/trg_l") == 0)
	{
		*side = CALIBRATION_LEFT;
		return true;
	}
	if(strcmp(path, "cal/trg_r") == 0)
	{
		*side = CALIBRATION_RIGHT;
		return true;
	}
	return false;
}

static bool pressure_path_side(const char *path, enum calibration_side *side)
{
	if(strcmp(path, "cal/prs_l") == 0)
	{
		*side = CALIBRATION_LEFT;
		return true;
	}
	if(strcmp(path, "cal/prs_r") == 0)
	{
		*side = CALIBRATION_RIGHT;
		return true;
	}
	return false;
}

static bool ibex_setting_path_id(const char *path, uint8_t *id)
{
	for(uint8_t candidate = 0; candidate < IBEX_SETTING_COUNT; ++candidate)
	{
		const char *candidate_path = ibex_setting_persist_path(candidate);

		if(candidate_path != NULL && strcmp(path, candidate_path) == 0)
		{
			*id = candidate;
			return true;
		}
	}
	return false;
}

static bool nvs_offset_path(const char *path, off_t *offset)
{
#if FIXED_PARTITION_EXISTS(storage_partition)
	unsigned int parsed;
	char tail;

	if(sscanf(path, "nvs/%x%c", &parsed, &tail) != 1)
	{
		return false;
	}
	if(parsed >= PARTITION_SIZE(storage_partition) || (parsed % sizeof(uint32_t)) != 0U)
	{
		return false;
	}

	*offset = parsed;
	return true;
#else
	ARG_UNUSED(path);
	ARG_UNUSED(offset);
	return false;
#endif
}

static bool nvs_read_path(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
#if FIXED_PARTITION_EXISTS(storage_partition)
	const struct flash_area *fa;
	off_t offset;
	size_t read_len;
	int err;

	if(strcmp(path, "nvs/info") == 0)
	{
		if(capacity < VALVE_NVS_INFO_SIZE)
		{
			return false;
		}
		sys_put_le32(PARTITION_SIZE(storage_partition), &buf[0]);
		sys_put_le32(VALVE_NVS_CHUNK_SIZE, &buf[4]);
		*len = VALVE_NVS_INFO_SIZE;
		return true;
	}
	if(!nvs_offset_path(path, &offset) || capacity < VALVE_NVS_CHUNK_SIZE)
	{
		return false;
	}

	err = flash_area_open(PARTITION_ID(storage_partition), &fa);
	if(err)
	{
		LOG_ERR("failed to open CFW NVS partition: %d", err);
		return false;
	}

	read_len = MIN(VALVE_NVS_CHUNK_SIZE, PARTITION_SIZE(storage_partition) - offset);
	err = flash_area_read(fa, offset, buf, read_len);
	flash_area_close(fa);
	if(err)
	{
		LOG_ERR("failed to read CFW NVS offset 0x%04x: %d", (unsigned int)offset, err);
		return false;
	}

	*len = read_len;
	return true;
#else
	ARG_UNUSED(path);
	ARG_UNUSED(buf);
	ARG_UNUSED(capacity);
	ARG_UNUSED(len);
	return false;
#endif
}

bool valve_settings_read(const char *path, uint8_t *buf, size_t capacity, size_t *len)
{
	if(remap_path(path))
	{
		return remap_read(path, buf, capacity, len);
	}
	if(sc26re_cfg_path(path))
	{
		return sc26re_cfg_read(path, buf, capacity, len);
	}
	if(strcmp(path, "sc26re/host") == 0 && capacity >= 4)
	{
		sys_put_le32(valve_feature_ms_since_host_traffic(), buf);
		*len = 4;
		return true;
	}
	if(strcmp(path, "sc26re/gm_state") == 0 && capacity >= 22)
	{
		struct gyro_mouse_status st;

		gyro_mouse_status(&st);
		buf[0] = st.enabled ? 1U : 0U;
		buf[1] = st.cal_state;
		sys_put_le32((uint32_t)st.cal_counts, &buf[2]);
		sys_put_le32((uint32_t)st.counts_per_360, &buf[6]);
		sys_put_le32(st.sent_abs_x, &buf[10]);
		sys_put_le32(st.sent_abs_y, &buf[14]);
		sys_put_le32(st.sent_reports, &buf[18]);
		*len = 22;
		return true;
	}
	if(strcmp(path, "sc26re/imu_stats") == 0 && capacity >= 20)
	{
		uint32_t st[5];

		imu_fast_stats(st);
		for(size_t i = 0; i < 5; ++i)
		{
			sys_put_le32(st[i], &buf[4 * i]);
		}
		*len = 20;
		return true;
	}
	if(strcmp(path, "sc26re/boot") == 0 && capacity >= 28)
	{
		/* why the controller last started: RESETREAS, uptime, crash record */
		const struct power_crash_info *c = power_last_crash();

		memset(buf, 0, 28);
		sys_put_le32(power_boot_reset_reason(), &buf[0]);
		sys_put_le32((uint32_t)k_uptime_get(), &buf[4]);
		if(c != NULL)
		{
			buf[8] = 1;
			buf[9] = (uint8_t)c->reason;
			sys_put_le32(c->pc, &buf[10]);
			sys_put_le32(c->lr, &buf[14]);
			memcpy(&buf[18], c->thread, 10);
		}
		*len = 28;
		return true;
	}
	if(strcmp(path, "sc26re/awake") == 0 && capacity >= 1)
	{
		/* report meter keep-alive: no auto power-off while the page is open */
		meter_awake_ms = k_uptime_get();
		buf[0] = 1;
		*len = 1;
		return true;
	}
	if(strcmp(path, "sc26re/gyro_full_cal") == 0 && capacity >= 7)
	{
		uint8_t reason, moves;
		uint32_t buttons;

		imu_full_cal_diag(&reason, &buttons, &moves);
		buf[0] = imu_full_cal_step();
		buf[1] = reason;
		sys_put_le32(buttons, &buf[2]);
		buf[6] = moves;
		*len = 7;
		return true;
	}
	if(strcmp(path, "sc26re/gyro_cal") == 0 && capacity >= 13)
	{
		int16_t sd[3];
		int16_t spread[3];

		buf[0] = imu_gyro_cal_state();
		imu_gyro_cal_result(sd, spread);
		for(size_t i = 0; i < 3; ++i)
		{
			sys_put_le16((uint16_t)sd[i], &buf[1 + 2 * i]);
			sys_put_le16((uint16_t)spread[i], &buf[7 + 2 * i]);
		}
		*len = 13;
		return true;
	}
	if(strcmp(path, SC26RE_CAPSENSE_PATH) == 0)
	{
		if(capacity < OLYMPUS_CAPSENSE_SIZE)
		{
			return false;
		}
		olympus_capsense_get(buf);
		*len = OLYMPUS_CAPSENSE_SIZE;
		if(capacity >= 2U * OLYMPUS_CAPSENSE_SIZE)
		{
			/* then the user's own values, 0 = default */
			olympus_capsense_get_user(&buf[OLYMPUS_CAPSENSE_SIZE]);
			*len = 2U * OLYMPUS_CAPSENSE_SIZE;
		}
		return true;
	}
	enum calibration_side side;
	uint8_t setting_id;
	int16_t setting_value;
	int slot = esb_bond_slot(path);

	if(slot >= 0)
	{
		const struct esb_bond *bond = &esb_bonds[slot].staged;

		if(capacity < sizeof(bond->data))
		{
			return false;
		}
		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		/* A forgotten slot (all-zero tombstone) must read as "no bond", like a missing
		 * setting: Steam took the zero record for a bond to an unknown Puck and asked
		 * to pair again after every reconnect. */
		if(!bond->valid || !esb_bond_material_valid(bond->data))
		{
			k_mutex_unlock(&esb_settings_mutex);
			return false;
		}
		memcpy(buf, bond->data, sizeof(bond->data));
		k_mutex_unlock(&esb_settings_mutex);
		*len = sizeof(bond->data);
		return true;
	}

	if(strcmp(path, VALVE_HAPTIC_BOOT_LEVEL_PATH) == 0)
	{
		if(capacity < 1U)
		{
			return false;
		}
		buf[0] = haptic_boot_level;
		*len = 1U;
		return true;
	}

	if(strcmp(path, VALVE_WIRELESS_TRANSPORT_PATH) == 0)
	{
		if(capacity < sizeof(wireless_transport))
		{
			return false;
		}
		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		if(!wireless_transport_valid)
		{
			k_mutex_unlock(&esb_settings_mutex);
			return false;
		}
		buf[0] = wireless_transport;
		k_mutex_unlock(&esb_settings_mutex);
		*len = sizeof(wireless_transport);
		return true;
	}

	if(nvs_read_path(path, buf, capacity, len))
	{
		return true;
	}

	if(ibex_setting_path_id(path, &setting_id))
	{
		if(capacity < sizeof(setting_value) || !ibex_setting_get(setting_id, &setting_value))
		{
			return false;
		}
		sys_put_le16((uint16_t)setting_value, buf);
		*len = sizeof(setting_value);
		return true;
	}

	if(calibration_path_side(path, &side))
	{
		return calibration_read_trigger(side, buf, capacity, len);
	}
	if(pressure_path_side(path, &side))
	{
		return calibration_read_pressure(side, buf, capacity, len);
	}

	if(imu_settings_read(path, buf, capacity, len))
	{
		return true;
	}

	return false;
}

static int nvs_write_path(const char *path, const uint8_t *value, size_t len)
{
#if FIXED_PARTITION_EXISTS(storage_partition)
	const struct flash_area *fa;
	off_t offset;
	size_t expected_len;
	int err;

	if(!nvs_offset_path(path, &offset))
	{
		return -ENOENT;
	}
	expected_len = MIN(VALVE_NVS_CHUNK_SIZE, PARTITION_SIZE(storage_partition) - offset);
	if((offset % VALVE_NVS_CHUNK_SIZE) != 0U ||
	   len != expected_len ||
	   (len % sizeof(uint32_t)) != 0U)
	{
		return -EINVAL;
	}

	err = flash_area_open(PARTITION_ID(storage_partition), &fa);
	if(err)
	{
		return err;
	}
	err = flash_area_write(fa, offset, value, len);
	flash_area_close(fa);
	if(err)
	{
		LOG_ERR("failed to write CFW NVS offset 0x%04x: %d", (unsigned int)offset, err);
		return err;
	}
	return 0;
#else
	ARG_UNUSED(path);
	ARG_UNUSED(value);
	ARG_UNUSED(len);
	return -ENOENT;
#endif
}

int valve_settings_stage(const char *path, const uint8_t *value, size_t len)
{
	enum calibration_side side;
	uint8_t setting_id;
	int slot = esb_bond_slot(path);
	int err;

	if(remap_path(path))
	{
		return remap_stage(path, value, len);
	}
	if(sc26re_cfg_path(path))
	{
		return sc26re_cfg_stage(path, value, len);
	}
	if(strcmp(path, SC26RE_CAPSENSE_PATH) == 0)
	{
		/* applied live; COMMIT stores it */
		return olympus_capsense_set(value, len);
	}

	if(slot >= 0)
	{
		struct esb_bond_slot *bond_slot = &esb_bonds[slot];

		if(len != sizeof(bond_slot->staged.data))
		{
			return -EINVAL;
		}
		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		memcpy(bond_slot->staged.data, value, sizeof(bond_slot->staged.data));
		bond_slot->staged.valid = true;
		bond_slot->dirty = true;
		k_mutex_unlock(&esb_settings_mutex);
		LOG_INF("staged ESB bond slot %d over feature settings", slot);
		return 0;
	}

	if(strcmp(path, VALVE_HAPTIC_BOOT_LEVEL_PATH) == 0)
	{
		if(len < 1U || value[0] > 2U)
		{
			return -EINVAL;
		}
		haptic_boot_level = value[0];
		haptic_boot_level_valid = true;
		return 0;
	}

	if(strcmp(path, VALVE_WIRELESS_TRANSPORT_PATH) == 0)
	{
		k_spinlock_key_t key;
		int selected_slot;
		bool selected_bond_available;

		if(len != sizeof(wireless_transport))
		{
			return -EINVAL;
		}
		selected_slot = esb_bond_slot_for_transport(value[0]);
		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		key = k_spin_lock(&active_esb_bond_lock);
		selected_bond_available =
		    selected_slot >= 0 && active_esb_bond_available_locked(selected_slot);
		k_spin_unlock(&active_esb_bond_lock, key);
		if(value[0] != 0 && !selected_bond_available)
		{
			k_mutex_unlock(&esb_settings_mutex);
			return -EINVAL;
		}
		wireless_transport = value[0];
		wireless_transport_valid = true;
		wireless_transport_dirty = true;
		k_mutex_unlock(&esb_settings_mutex);
		return 0;
	}

	err = nvs_write_path(path, value, len);
	if(err != -ENOENT)
	{
		return err;
	}

	if(calibration_path_side(path, &side))
	{
		if(IS_ENABLED(CONFIG_IBEX_TRIGGER_LOCK_HOST_CAL))
		{
			/* Steam re-sends trigger records it keeps on the PC (often with a wrong
			 * direction flag); the controller's own calibration stays in charge.
			 * Use the report meter's trigger calibration / zero instead. */
			LOG_INF("ignored host write to %s (trigger calibration locked)", path);
			return 0;
		}
		return calibration_stage_trigger(side, value, len);
	}
	if(pressure_path_side(path, &side))
	{
		return calibration_stage_pressure(side, value, len);
	}

	if(ibex_setting_path_id(path, &setting_id))
	{
		if(len != sizeof(int16_t))
		{
			return -EINVAL;
		}
		return ibex_setting_set(setting_id, (int16_t)sys_get_le16(value));
	}

	err = imu_settings_stage(path, value, len);
	if(err != -ENOENT)
	{
		return err;
	}

	return -ENOENT;
}

static int nvs_commit_path(const char *path)
{
#if FIXED_PARTITION_EXISTS(storage_partition)
	const struct flash_area *fa;
	int err;

	if(strcmp(path, "nvs/erase") != 0)
	{
		return -ENOENT;
	}

	err = flash_area_open(PARTITION_ID(storage_partition), &fa);
	if(err)
	{
		return err;
	}
	err = flash_area_erase(fa, 0, PARTITION_SIZE(storage_partition));
	flash_area_close(fa);
	if(err)
	{
		LOG_ERR("failed to erase CFW NVS partition: %d", err);
		return err;
	}
	LOG_INF("erased CFW NVS partition for feature restore");
	return 0;
#else
	ARG_UNUSED(path);
	return -ENOENT;
#endif
}

static int commit_setting_path(const char *path)
{
	enum calibration_side side;
	uint8_t setting_id;
	uint8_t encoded[sizeof(int16_t)];
	int16_t setting_value;
	int slot = esb_bond_slot(path);
	int err;

	if(remap_path(path))
	{
		return remap_commit(path);
	}
	if(sc26re_cfg_path(path))
	{
		return sc26re_cfg_commit(path);
	}
	if(strcmp(path, SC26RE_CAPSENSE_PATH) == 0)
	{
		return olympus_capsense_save();
	}

	if(slot >= 0)
	{
		struct esb_bond_slot *bond_slot = &esb_bonds[slot];
		k_spinlock_key_t key;
		uint32_t bond_id[2];

		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		if(!bond_slot->dirty)
		{
			k_mutex_unlock(&esb_settings_mutex);
			return 0;
		}
		err = settings_save_one(esb_bond_paths[slot], bond_slot->staged.data,
		                        sizeof(bond_slot->staged.data));
		if(err)
		{
			k_mutex_unlock(&esb_settings_mutex);
			LOG_ERR("failed to commit ESB bond slot %d: %d", slot, err);
			return err;
		}
		bond_slot->dirty = false;
		key = k_spin_lock(&active_esb_bond_lock);
		bond_slot->active = bond_slot->staged;
		k_spin_unlock(&active_esb_bond_lock, key);
		bond_id[0] = sys_get_le32(&bond_slot->staged.data[0]);
		bond_id[1] = sys_get_le32(&bond_slot->staged.data[4]);
		k_mutex_unlock(&esb_settings_mutex);
		LOG_INF("committed ESB bond slot %d %08x/%08x", slot, bond_id[0], bond_id[1]);
		(void)haptics_play_system_sound(HAPTICS_SOUND_PAIRED);
		notify_esb_bond_changed();
		return 0;
	}

	if(strcmp(path, VALVE_HAPTIC_BOOT_LEVEL_PATH) == 0)
	{
		err = settings_save_one(VALVE_HAPTIC_BOOT_LEVEL_PATH, &haptic_boot_level,
		                        sizeof(haptic_boot_level));
		LOG_INF("haptic boot level %u (%d)", haptic_boot_level, err);
		return err;
	}

	if(strcmp(path, VALVE_WIRELESS_TRANSPORT_PATH) == 0)
	{
		k_spinlock_key_t key;

		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		if(!wireless_transport_dirty)
		{
			k_mutex_unlock(&esb_settings_mutex);
			return 0;
		}
		err = settings_save_one(VALVE_WIRELESS_TRANSPORT_PATH, &wireless_transport,
		                        sizeof(wireless_transport));
		if(err)
		{
			k_mutex_unlock(&esb_settings_mutex);
			LOG_ERR("failed to commit wireless transport: %d", err);
			return err;
		}
		wireless_transport_dirty = false;
		key = k_spin_lock(&active_esb_bond_lock);
		active_wireless_transport = wireless_transport;
		active_wireless_transport_valid = wireless_transport_valid;
		k_spin_unlock(&active_esb_bond_lock, key);
		k_mutex_unlock(&esb_settings_mutex);
		notify_esb_bond_changed();
		return 0;
	}

	err = nvs_commit_path(path);
	if(err != -ENOENT)
	{
		return err;
	}

	if(calibration_path_side(path, &side))
	{
		if(IS_ENABLED(CONFIG_IBEX_TRIGGER_LOCK_HOST_CAL))
		{
			return 0;
		}
		return calibration_commit_trigger(side);
	}
	if(pressure_path_side(path, &side))
	{
		return calibration_commit_pressure(side);
	}

	if(ibex_setting_path_id(path, &setting_id))
	{
		if(!ibex_setting_get(setting_id, &setting_value))
		{
			return -EINVAL;
		}
		sys_put_le16((uint16_t)setting_value, encoded);
		return settings_save_one(path, encoded, sizeof(encoded));
	}

	err = imu_settings_commit(path);
	if(err != -ENOENT)
	{
		return err;
	}

	return -ENOENT;
}

int valve_settings_commit(const char *path)
{
	int err;

	k_mutex_lock(&valve_settings_commit_mutex, K_FOREVER);
	err = commit_setting_path(path);
	k_mutex_unlock(&valve_settings_commit_mutex);
	return err;
}

/*
 * DELETE_SETTING (0xF0). Steam uses it to forget a Puck pairing: the bond
 * slot is wiped from flash and from RAM, and if it was the selected
 * wireless transport the controller falls back to the other slot (or none),
 * which makes the ESB link drop and stop reconnecting to that Puck.
 */
static int delete_setting_path(const char *path)
{
	k_spinlock_key_t key;
	int slot = esb_bond_slot(path);
	int err;

	if(slot >= 0)
	{
		struct esb_bond_slot *bond_slot = &esb_bonds[slot];
		int selected;

		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		err = IS_ENABLED(CONFIG_SETTINGS) ? settings_delete(esb_bond_paths[slot]) : 0;
		if(err)
		{
			k_mutex_unlock(&esb_settings_mutex);
			LOG_ERR("failed to delete ESB bond slot %d: %d", slot, err);
			return err;
		}
		/* remember it, so the stock bond is not imported again on the next boot */
		esb_forgot_mask |= BIT(slot);
		esb_forgot_save_locked();
		memset(&bond_slot->staged, 0, sizeof(bond_slot->staged));
		bond_slot->dirty = false;

		selected = wireless_transport_valid ? esb_bond_slot_for_transport(wireless_transport) : -1;
		if(selected == slot)
		{
			select_available_wireless_transport();
			if(IS_ENABLED(CONFIG_SETTINGS))
			{
				if(wireless_transport_valid)
				{
					(void)settings_save_one(VALVE_WIRELESS_TRANSPORT_PATH, &wireless_transport,
					                        sizeof(wireless_transport));
				}
				else
				{
					(void)settings_delete(VALVE_WIRELESS_TRANSPORT_PATH);
				}
			}
			wireless_transport_dirty = false;
		}

		key = k_spin_lock(&active_esb_bond_lock);
		bond_slot->active = bond_slot->staged;
		active_wireless_transport = wireless_transport;
		active_wireless_transport_valid = wireless_transport_valid;
		k_spin_unlock(&active_esb_bond_lock, key);
		k_mutex_unlock(&esb_settings_mutex);
		LOG_INF("deleted ESB bond slot %d (forget Puck)", slot);
		notify_esb_bond_changed();
		return 0;
	}

	if(strcmp(path, VALVE_WIRELESS_TRANSPORT_PATH) == 0)
	{
		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		if(IS_ENABLED(CONFIG_SETTINGS))
		{
			(void)settings_delete(VALVE_WIRELESS_TRANSPORT_PATH);
		}
		wireless_transport_valid = false;
		wireless_transport_dirty = false;
		select_available_wireless_transport();
		key = k_spin_lock(&active_esb_bond_lock);
		active_wireless_transport = wireless_transport;
		active_wireless_transport_valid = wireless_transport_valid;
		k_spin_unlock(&active_esb_bond_lock, key);
		k_mutex_unlock(&esb_settings_mutex);
		notify_esb_bond_changed();
		return 0;
	}

	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		return -ENOENT;
	}
	err = settings_delete(path);
	LOG_INF("deleted setting %s: %d", path, err);
	return err;
}

int valve_settings_delete(const char *path)
{
	int err;

	k_mutex_lock(&valve_settings_commit_mutex, K_FOREVER);
	err = delete_setting_path(path);
	k_mutex_unlock(&valve_settings_commit_mutex);
	return err;
}

#define FACTORY_RESET_MAX_KEYS 48
#define FACTORY_RESET_KEY_MAX 64

struct factory_reset_collect
{
	const char *subtree;
	char (*names)[FACTORY_RESET_KEY_MAX];
	size_t count;
};

static int factory_reset_collect_cb(const char *key, size_t len, settings_read_cb read_cb,
                                    void *cb_arg, void *param)
{
	struct factory_reset_collect *c = param;

	ARG_UNUSED(len);
	ARG_UNUSED(read_cb);
	ARG_UNUSED(cb_arg);
	if(c->count >= FACTORY_RESET_MAX_KEYS)
	{
		return 1;
	}
	if(key != NULL && key[0] != '\0')
	{
		snprintf(c->names[c->count], FACTORY_RESET_KEY_MAX, "%s/%s", c->subtree, key);
	}
	else
	{
		snprintf(c->names[c->count], FACTORY_RESET_KEY_MAX, "%s", c->subtree);
	}
	c->count++;
	return 0;
}

int valve_settings_factory_reset(void)
{
	static const char *const subtrees[] = {
		"cal",                  /* trigger/stick/pressure/IMU calibration, battery trim */
		"settings/haptics",     /* persisted Steam (ibex registry) settings */
		"user",                 /* wireless transport, boot chime level */
		"radio",                /* BLE/ESB personality */
		"bt",                   /* Bluetooth bonds and identity */
	};
	static char names[FACTORY_RESET_MAX_KEYS][FACTORY_RESET_KEY_MAX];
	int deleted = 0;

	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		return -ENOTSUP;
	}

	k_mutex_lock(&valve_settings_commit_mutex, K_FOREVER);
	for(size_t t = 0; t < ARRAY_SIZE(subtrees); ++t)
	{
		struct factory_reset_collect c = {
			.subtree = subtrees[t],
			.names = names,
			.count = 0,
		};

		(void)settings_load_subtree_direct(subtrees[t], factory_reset_collect_cb, &c);
		for(size_t i = 0; i < c.count; ++i)
		{
			int err = settings_delete(names[i]);

			LOG_INF("factory reset: delete %s: %d", names[i], err);
			if(err == 0)
			{
				deleted++;
			}
		}
	}
	/* Puck pairings: delete, and mark both slots forgotten so the stock bonds in
	 * Valve's storage are not imported again. */
	for(size_t slot = 0; slot < ARRAY_SIZE(esb_bond_paths); ++slot)
	{
		int err = settings_delete(esb_bond_paths[slot]);

		LOG_INF("factory reset: forget %s: %d", esb_bond_paths[slot], err);
		if(err == 0)
		{
			deleted++;
		}
	}
	k_mutex_lock(&esb_settings_mutex, K_FOREVER);
	esb_forgot_mask = BIT_MASK(ARRAY_SIZE(esb_bond_paths));
	esb_forgot_save_locked();
	k_mutex_unlock(&esb_settings_mutex);
	k_mutex_unlock(&valve_settings_commit_mutex);
	ibex_settings_registry_reset_defaults();
	LOG_INF("factory reset: %d entries removed", deleted);
	return deleted;
}

int valve_settings_save_esb_bond(uint8_t slot, const uint8_t *bond, size_t len, bool select)
{
	k_spinlock_key_t key;
	uint8_t transport;
	int err;

	if(slot >= ARRAY_SIZE(esb_bonds) ||
	   bond == NULL ||
	   len != VALVE_ESB_BOND_SIZE ||
	   (select && !esb_bond_material_valid(bond)))
	{
		return -EINVAL;
	}

	err = valve_settings_stage(esb_bond_paths[slot], bond, len);
	if(err)
	{
		return err;
	}
	if(!IS_ENABLED(CONFIG_SETTINGS))
	{
		struct esb_bond_slot *bond_slot = &esb_bonds[slot];

		k_mutex_lock(&esb_settings_mutex, K_FOREVER);
		bond_slot->dirty = false;
		key = k_spin_lock(&active_esb_bond_lock);
		bond_slot->active = bond_slot->staged;
		if(select)
		{
			transport = slot == 0 ? VALVE_ESB_SLOT_0_TRANSPORT : VALVE_ESB_SLOT_1_TRANSPORT;
			wireless_transport = transport;
			wireless_transport_valid = true;
			wireless_transport_dirty = false;
			active_wireless_transport = transport;
			active_wireless_transport_valid = true;
		}
		k_spin_unlock(&active_esb_bond_lock, key);
		k_mutex_unlock(&esb_settings_mutex);
		notify_esb_bond_changed();
		return 0;
	}
	err = valve_settings_commit(esb_bond_paths[slot]);
	if(err || !select)
	{
		return err;
	}

	transport = slot == 0 ? VALVE_ESB_SLOT_0_TRANSPORT : VALVE_ESB_SLOT_1_TRANSPORT;
	err = valve_settings_stage(VALVE_WIRELESS_TRANSPORT_PATH, &transport, sizeof(transport));
	if(err)
	{
		return err;
	}
	return valve_settings_commit(VALVE_WIRELESS_TRANSPORT_PATH);
}

uint8_t valve_settings_haptic_boot_level(void)
{
	return haptic_boot_level;
}

bool valve_settings_active_wireless_transport(uint8_t *transport)
{
	k_spinlock_key_t key = k_spin_lock(&active_esb_bond_lock);
	bool valid = active_wireless_transport_valid;

	*transport = active_wireless_transport;
	k_spin_unlock(&active_esb_bond_lock, key);
	return valid;
}
