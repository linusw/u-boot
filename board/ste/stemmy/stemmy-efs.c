// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung EFS (Extended File System) stores persistent device data on many
 * Samsung phones. On the Ux500 devices, it is an ext4 filesystem containing
 * the Wi-Fi and Bluetooth addresses.
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
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dm/uclass.h>

#include "stemmy-efs.h"

#define EFS_PARTITION	11
#define MAC_ADDR_LEN	17

static struct blk_desc *stemmy_efs_device(void)
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

		/* EFS is in the user area, not an eMMC hardware boot area. */
		desc = mmc_get_blk_desc(mmc);
		if (!desc || blk_dselect_hwpart(desc, 0))
			continue;

		return desc;
	}

	return NULL;
}

static int stemmy_efs_read_mac(struct blk_desc *desc, const char *path, u8 *mac)
{
	char buf[MAC_ADDR_LEN + 2];
	loff_t len;
	int i, ret;

	if (!desc)
		return -ENODEV;

	/* fs_read() closes the filesystem, so reopen it for each address. */
	ret = fs_set_blk_dev_with_part(desc, EFS_PARTITION);
	if (ret)
		return ret;

	if (fs_get_type() != FS_TYPE_EXT) {
		fs_close();
		return -EINVAL;
	}

	ret = fs_read(path, (ulong)buf, 0, sizeof(buf), &len);
	if (ret)
		return ret;

	/* Samsung stores text, optionally terminated by a newline. */
	if (len != MAC_ADDR_LEN &&
	    (len != MAC_ADDR_LEN + 1 || buf[MAC_ADDR_LEN] != '\n'))
		return -EINVAL;

	for (i = 0; i < ETH_ALEN; i++) {
		if (i && buf[3 * i - 1] != ':')
			return -EINVAL;
		if (hex2bin(mac + i, buf + 3 * i, 1))
			return -EINVAL;
	}

	if (is_zero_ethaddr(mac) || is_broadcast_ethaddr(mac))
		return -EINVAL;

	return 0;
}

static void stemmy_efs_fixup_mac(void *fdt, struct blk_desc *desc,
				 const char *path, const char *compatible,
				 bool bluetooth)
{
	const char *property = bluetooth ? "local-bd-address" : "local-mac-address";
	const char *name = bluetooth ? "Bluetooth BD" : "WiFi MAC";
	u8 mac[ETH_ALEN];
	int node, ret, i;

	ret = stemmy_efs_read_mac(desc, path, mac);
	if (ret || (!bluetooth && is_multicast_ethaddr(mac))) {
		printf("EFS %s missing\n", name);
		return;
	}

	printf("EFS %s: %pM\n", name, mac);

	node = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (node < 0)
		return;

	/* local-bd-address uses Bluetooth (least significant byte first) order. */
	if (bluetooth) {
		for (i = 0; i < ETH_ALEN / 2; i++)
			swap(mac[i], mac[ETH_ALEN - i - 1]);
	} else if (fdt_getprop(fdt, node, "mac-address", NULL)) {
		/* Linux prefers mac-address if it is already present. */
		ret = fdt_setprop(fdt, node, "mac-address", mac, sizeof(mac));
		if (ret)
			goto err;
	}

	ret = fdt_setprop(fdt, node, property, mac, sizeof(mac));
	if (!ret)
		return;

err:
	log_warning("Cannot set %s: %s\n", property, fdt_strerror(ret));
}

void stemmy_efs_fixup(void *fdt)
{
	struct blk_desc *desc = stemmy_efs_device();

	stemmy_efs_fixup_mac(fdt, desc, "/wifi/.mac.info",
			     "brcm,bcm4329-fmac", false);
	stemmy_efs_fixup_mac(fdt, desc, "/bluetooth/bt_addr",
			     "brcm,bcm4330-bt", true);
}
