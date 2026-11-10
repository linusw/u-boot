// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung stores persistent radio addresses in several locations depending
 * on the firmware and device. Pass them to the operating system through the
 * device tree when they can be found on the internal eMMC.
 *
 * Copyright (C) 2026 Linus Walleij <linusw@kernel.org>
 */

#include <blk.h>
#include <dm.h>
#include <fdt_support.h>
#include <fs.h>
#include <log.h>
#include <mmc.h>
#include <net-common.h>
#include <part.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dm/uclass.h>
#include <linux/ctype.h>

#include "stemmy-radio.h"

#define RADIO_ADDR_BUF_SIZE	512

enum stemmy_radio_partition {
	STEMMY_PART_EFS,
	STEMMY_PART_DATAFS,
	STEMMY_PART_COUNT,
};

struct stemmy_radio_source {
	enum stemmy_radio_partition partition;
	const char *path;
};

struct stemmy_radio_storage {
	struct blk_desc *desc;
	int part[STEMMY_PART_COUNT];
};

static const char * const stemmy_partition_names[STEMMY_PART_COUNT] = {
	[STEMMY_PART_EFS] = "EFS",
	[STEMMY_PART_DATAFS] = "DATAFS",
};

static const struct stemmy_radio_source stemmy_wifi_sources[] = {
	{ STEMMY_PART_EFS, "/wifi/.mac.info" },
	{ STEMMY_PART_EFS, "/wifi/.mac.cob" },
	{ STEMMY_PART_EFS, "/imei/.nvmac.info" },
	{ STEMMY_PART_DATAFS, "/.nvmac.info" },
	{ STEMMY_PART_DATAFS, "/.mac.info" },
	{ STEMMY_PART_DATAFS, "/misc/wifi/.mac.info" },
};

static const struct stemmy_radio_source stemmy_bt_sources[] = {
	{ STEMMY_PART_EFS, "/bluetooth/bt_addr" },
	{ STEMMY_PART_EFS, "/bluetooth/addr.txt" },
};

static struct blk_desc *stemmy_radio_device(void)
{
	struct blk_desc *desc;
	struct udevice *dev;
	struct uclass *uc;
	struct mmc *mmc;

	if (uclass_get(UCLASS_MMC, &uc))
		return NULL;

	uclass_foreach_dev(dev, uc) {
		/* Android's mmcblk0 is not necessarily U-Boot's mmc 0. */
		if (!dev_read_bool(dev, "non-removable") || device_probe(dev))
			continue;

		mmc = mmc_get_mmc_dev(dev);
		if (!mmc || mmc_init(mmc) || !IS_MMC(mmc))
			continue;

		/* Persistent data is in the user area, not an eMMC boot area. */
		desc = mmc_get_blk_desc(mmc);
		if (!desc || blk_dselect_hwpart(desc, 0))
			continue;

		return desc;
	}

	return NULL;
}

static bool stemmy_addr_tail_valid(const char *buf, size_t len, size_t pos)
{
	for (; pos < len; pos++) {
		if (buf[pos] && !isspace((unsigned char)buf[pos]))
			return false;
	}

	return true;
}

static int stemmy_parse_radio_addr(char *buf, size_t len, u8 *addr)
{
	const char *prefix = "bt_macaddr:";
	const char *str = buf;
	size_t pos = 0;
	bool prefixed = false;
	int i;

	str = strstr(buf, prefix);
	if (str) {
		str += strlen(prefix);
		pos = str - buf;
		prefixed = true;
	} else {
		str = buf;
		while (pos < len && isspace((unsigned char)str[pos]))
			pos++;
		str += pos;
	}

	if (pos + 17 <= len && str[2] == ':' && str[5] == ':' &&
	    str[8] == ':' && str[11] == ':' && str[14] == ':') {
		for (i = 0; i < ETH_ALEN; i++) {
			if (hex2bin(addr + i, str + 3 * i, 1))
				return -EINVAL;
		}
		pos += 17;
	} else if (pos + 12 <= len) {
		if (hex2bin(addr, str, ETH_ALEN))
			return -EINVAL;
		pos += 12;
	} else {
		return -EINVAL;
	}

	if (!prefixed && !stemmy_addr_tail_valid(buf, len, pos))
		return -EINVAL;

	return 0;
}

static int stemmy_read_radio_addr(struct blk_desc *desc, int part,
				  const char *path, u8 *addr)
{
	char buf[RADIO_ADDR_BUF_SIZE + 1];
	loff_t len;
	int ret;

	ret = fs_set_blk_dev_with_part(desc, part);
	if (ret)
		return ret;

	if (fs_get_type() != FS_TYPE_EXT) {
		fs_close();
		return -EINVAL;
	}

	/* fs_read() closes the filesystem even when the read fails. */
	ret = fs_read(path, (ulong)buf, 0, RADIO_ADDR_BUF_SIZE, &len);
	if (ret)
		return ret;
	if (!len || len > RADIO_ADDR_BUF_SIZE)
		return -EINVAL;

	buf[len] = '\0';

	return stemmy_parse_radio_addr(buf, len, addr);
}

static bool stemmy_radio_addr_valid(const u8 *addr, bool bluetooth)
{
	static const u8 wifi_placeholders[][ETH_ALEN] = {
		{ 0x00, 0x90, 0x4c, 0xc5, 0x12, 0x38 },
		{ 0x00, 0x12, 0x34, 0x00, 0x0d, 0xef },
	};
	static const u8 bluetooth_placeholder[ETH_ALEN] = {
		0x00, 0x12, 0x47, 0x01, 0x23, 0x45,
	};
	size_t i;

	if (is_zero_ethaddr(addr) || is_broadcast_ethaddr(addr))
		return false;
	if (bluetooth)
		return !!memcmp(addr, bluetooth_placeholder, ETH_ALEN);
	if (is_multicast_ethaddr(addr))
		return false;

	for (i = 0; i < ARRAY_SIZE(wifi_placeholders); i++) {
		if (!memcmp(addr, wifi_placeholders[i], ETH_ALEN))
			return false;
	}

	return true;
}

static int stemmy_find_radio_addr(struct stemmy_radio_storage *storage,
				  const struct stemmy_radio_source *sources,
				  size_t count, bool bluetooth, u8 *addr)
{
	size_t i;
	int part, ret;

	for (i = 0; i < count; i++) {
		part = storage->part[sources[i].partition];
		if (part < 0)
			continue;

		ret = stemmy_read_radio_addr(storage->desc, part,
					     sources[i].path, addr);
		if (ret || !stemmy_radio_addr_valid(addr, bluetooth))
			continue;

		log_debug("Read radio address from %s:%s\n",
			  stemmy_partition_names[sources[i].partition],
			  sources[i].path);
		return 0;
	}

	return -ENOENT;
}

static void stemmy_fixup_radio_addr(void *fdt,
				    struct stemmy_radio_storage *storage,
				    const struct stemmy_radio_source *sources,
				    size_t count, const char *compatible,
				    bool bluetooth)
{
	const char *property = bluetooth ? "local-bd-address" :
					       "local-mac-address";
	const char *name = bluetooth ? "Bluetooth BD" : "WiFi MAC";
	u8 addr[ETH_ALEN];
	int node, ret, i;

	ret = stemmy_find_radio_addr(storage, sources, count, bluetooth, addr);
	if (ret) {
		log_warning("Persistent %s missing\n", name);
		return;
	}

	log_info("Persistent %s: %pM\n", name, addr);

	node = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (node < 0)
		return;

	/* local-bd-address uses Bluetooth (least significant byte first) order. */
	if (bluetooth) {
		for (i = 0; i < ETH_ALEN / 2; i++)
			swap(addr[i], addr[ETH_ALEN - i - 1]);
	} else if (fdt_getprop(fdt, node, "mac-address", NULL)) {
		/* Linux prefers mac-address if it is already present. */
		ret = fdt_setprop(fdt, node, "mac-address", addr, sizeof(addr));
		if (ret)
			goto err;
	}

	ret = fdt_setprop(fdt, node, property, addr, sizeof(addr));
	if (!ret)
		return;

err:
	log_warning("Cannot set %s: %s\n", property, fdt_strerror(ret));
}

void stemmy_radio_fixup(void *fdt)
{
	struct stemmy_radio_storage storage;
	struct disk_partition info;
	int i;

	storage.desc = stemmy_radio_device();
	if (!storage.desc)
		return;

	for (i = 0; i < STEMMY_PART_COUNT; i++)
		storage.part[i] = part_get_info_by_name(storage.desc,
							stemmy_partition_names[i],
							&info);

	stemmy_fixup_radio_addr(fdt, &storage, stemmy_wifi_sources,
				ARRAY_SIZE(stemmy_wifi_sources),
				"brcm,bcm4329-fmac", false);
	stemmy_fixup_radio_addr(fdt, &storage, stemmy_bt_sources,
				ARRAY_SIZE(stemmy_bt_sources),
				"brcm,bcm4330-bt", true);
}
