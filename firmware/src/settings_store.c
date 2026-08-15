/*
 * Settings persistence over NVS. See settings_store.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "settings_store.h"

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <string.h>

/*
 * NVS ids. Append only, never reuse: an id that changes meaning between firmware versions reads a
 * broker hostname as a passphrase, and the device gives no clue that it happened.
 *
 * Prefixed because the vendor pack header claims startlingly generic names. These were briefly bare
 * `ID_*` constants, and `ID_PORT` collided with `#define ID_PORT (32)` — the PORT peripheral's
 * instance index in pic32cm6408pl10048.h. The compiler reported it as a syntax error *inside the pack
 * header*, which is a memorably unhelpful place to be sent for a name clash in this file.
 */
enum {
	SET_ID_SSID = 1,
	SET_ID_PASS,
	SET_ID_BROKER,
	SET_ID_CLIENT,
	SET_ID_USER,
	SET_ID_MQTTPASS,
	SET_ID_PORT,
	SET_ID_SEC,
	SET_ID_BRIGHTNESS,
	SET_ID_GAIN,	/* all three lamps as one record; they are calibrated together */
	SET_ID_TRANSPORT,
};

static struct nvs_fs fs;
static bool mounted;

void settings_defaults(struct wigwag_settings *s)
{
	memset(s, 0, sizeof(*s));

	/*
	 * From Kconfig, which firmware/credentials.conf supplies when present. Truncation here would
	 * be silent, so the sizes in settings.h are chosen to hold anything the protocols allow and a
	 * over-long Kconfig value is clipped by strncpy — acceptable because that value came from a
	 * developer editing a file, not from the field, and the console refuses over-long input.
	 */
	(void)strncpy(s->ssid, CONFIG_WIGWAG_SSID, sizeof(s->ssid) - 1U);
	(void)strncpy(s->pass, CONFIG_WIGWAG_PASS, sizeof(s->pass) - 1U);
	(void)strncpy(s->broker, CONFIG_WIGWAG_BROKER, sizeof(s->broker) - 1U);
	(void)strncpy(s->client, CONFIG_WIGWAG_CLIENT, sizeof(s->client) - 1U);
	(void)strncpy(s->user, CONFIG_WIGWAG_USER, sizeof(s->user) - 1U);
	(void)strncpy(s->mqttpass, CONFIG_WIGWAG_MQTTPASS, sizeof(s->mqttpass) - 1U);

	s->port = CONFIG_WIGWAG_PORT;
	s->sec = CONFIG_WIGWAG_SEC;
	s->transport = IS_ENABLED(CONFIG_WIGWAG_TRANSPORT_USB) ? WIGWAG_TRANSPORT_USB
							       : WIGWAG_TRANSPORT_WIFI;

	/*
	 * Full brightness and no per-lamp correction, matching what lamp_pwm.c starts with. A device
	 * that boots dark looks broken until the retained topic arrives (D88).
	 */
	s->brightness = 255;
	s->gain[LAMP_GREEN] = 255;
	s->gain[LAMP_YELLOW] = 255;
	s->gain[LAMP_RED] = 255;
}

static int mount(void)
{
	const struct device *flash = DEVICE_DT_GET(DT_NODELABEL(nvmctrl));
	struct flash_pages_info page;
	int ret;

	if (mounted) {
		return 0;
	}

	if (!device_is_ready(flash)) {
		return -ENODEV;
	}

	/*
	 * Sector size from the hardware rather than a constant, so this does not quietly break if the
	 * partition moves or a sibling part has different pages. NVS requires whole erase pages.
	 */
	ret = flash_get_page_info_by_offs(flash, PARTITION_OFFSET(storage_partition), &page);
	if (ret != 0) {
		return ret;
	}

	fs.flash_device = flash;
	fs.offset = PARTITION_OFFSET(storage_partition);
	fs.sector_size = page.size;
	fs.sector_count = PARTITION_SIZE(storage_partition) / page.size;

	ret = nvs_mount(&fs);
	if (ret != 0) {
		return ret;
	}

	mounted = true;

	return 0;
}

/** Read one string, leaving the default in place if it is absent. */
static void load_str(uint16_t id, char *dst, size_t cap)
{
	ssize_t n = nvs_read(&fs, id, dst, cap);

	if (n <= 0) {
		return;		/* never written, or unreadable: keep the default */
	}

	/*
	 * Defend against a record that is not NUL-terminated. NVS returns what was written, and a
	 * truncated or corrupt record would otherwise let a string run past its buffer.
	 */
	dst[cap - 1U] = '\0';
}

int settings_load(struct wigwag_settings *s)
{
	int ret;

	settings_defaults(s);

	ret = mount();
	if (ret != 0) {
		return ret;
	}

	load_str(SET_ID_SSID, s->ssid, sizeof(s->ssid));
	load_str(SET_ID_PASS, s->pass, sizeof(s->pass));
	load_str(SET_ID_BROKER, s->broker, sizeof(s->broker));
	load_str(SET_ID_CLIENT, s->client, sizeof(s->client));
	load_str(SET_ID_USER, s->user, sizeof(s->user));
	load_str(SET_ID_MQTTPASS, s->mqttpass, sizeof(s->mqttpass));

	(void)nvs_read(&fs, SET_ID_PORT, &s->port, sizeof(s->port));
	(void)nvs_read(&fs, SET_ID_SEC, &s->sec, sizeof(s->sec));
	(void)nvs_read(&fs, SET_ID_BRIGHTNESS, &s->brightness, sizeof(s->brightness));
	(void)nvs_read(&fs, SET_ID_GAIN, s->gain, sizeof(s->gain));
	(void)nvs_read(&fs, SET_ID_TRANSPORT, &s->transport, sizeof(s->transport));

	return 0;
}

static int save_str(uint16_t id, const char *v)
{
	ssize_t n = nvs_write(&fs, id, v, strlen(v) + 1U);

	return (n < 0) ? (int)n : 0;
}

int settings_save(const struct wigwag_settings *s)
{
	int ret = mount();

	if (ret != 0) {
		return ret;
	}

	/*
	 * Written one record at a time rather than as one blob, which is what makes NVS's
	 * unchanged-value skip useful: adjusting brightness does not rewrite the passphrase, so the
	 * partition wears in proportion to what actually changes.
	 *
	 * First error wins and stops the rest. A partial save is possible and is the honest outcome —
	 * the alternative would be pretending the whole set is transactional when it is not.
	 */
	ret = save_str(SET_ID_SSID, s->ssid);
	if (ret == 0) {
		ret = save_str(SET_ID_PASS, s->pass);
	}
	if (ret == 0) {
		ret = save_str(SET_ID_BROKER, s->broker);
	}
	if (ret == 0) {
		ret = save_str(SET_ID_CLIENT, s->client);
	}
	if (ret == 0) {
		ret = save_str(SET_ID_USER, s->user);
	}
	if (ret == 0) {
		ret = save_str(SET_ID_MQTTPASS, s->mqttpass);
	}
	if (ret == 0) {
		ssize_t n = nvs_write(&fs, SET_ID_PORT, &s->port, sizeof(s->port));

		ret = (n < 0) ? (int)n : 0;
	}
	if (ret == 0) {
		ssize_t n = nvs_write(&fs, SET_ID_SEC, &s->sec, sizeof(s->sec));

		ret = (n < 0) ? (int)n : 0;
	}
	if (ret == 0) {
		ssize_t n = nvs_write(&fs, SET_ID_BRIGHTNESS, &s->brightness, sizeof(s->brightness));

		ret = (n < 0) ? (int)n : 0;
	}
	if (ret == 0) {
		ssize_t n = nvs_write(&fs, SET_ID_GAIN, s->gain, sizeof(s->gain));

		ret = (n < 0) ? (int)n : 0;
	}
	if (ret == 0) {
		ssize_t n = nvs_write(&fs, SET_ID_TRANSPORT, &s->transport, sizeof(s->transport));

		ret = (n < 0) ? (int)n : 0;
	}

	return ret;
}

int settings_clear(struct wigwag_settings *s)
{
	int ret = mount();
	uint16_t id;

	settings_defaults(s);

	if (ret != 0) {
		return ret;
	}

	/*
	 * Delete every id rather than erasing the partition behind NVS's back. nvs_delete() appends a
	 * tombstone, which keeps the store consistent if power is lost partway through — erasing the
	 * sectors directly would leave NVS reading a partition it no longer recognises.
	 */
	for (id = SET_ID_SSID; id <= SET_ID_TRANSPORT; id++) {
		int e = nvs_delete(&fs, id);

		if (e != 0 && ret == 0) {
			ret = e;
		}
	}

	return ret;
}
