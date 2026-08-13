// SPDX-License-Identifier: GPL-2.0+
/* ST-Ericsson AB8500/AB8505 LDO AUX3 regulator */

#include <dm.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <linux/errno.h>
#include <power/ab8500.h>
#include <power/pmic.h>
#include <power/regulator.h>

#define AB8500_VAUX3_REGU	AB8500_REGU_CTRL2(0x0a)
#define AB8500_VAUX3_SEL	AB8500_REGU_CTRL2(0x21)
#define AB8500_VAUX3_EN_MASK	GENMASK(1, 0)
#define AB8500_VAUX3_EN		BIT(0)
#define AB8500_VAUX3_SEL_MASK	GENMASK(2, 0)

static const int ab8500_vaux3_voltages[] = {
	1200000, 1500000, 1800000, 2100000,
	2500000, 2750000, 2790000, 2910000,
};

static struct udevice *ab8500_regulator_pmic(struct udevice *dev)
{
	return dev->parent->parent;
}

static int ab8500_regulator_get_value(struct udevice *dev)
{
	int ret;

	ret = pmic_reg_read(ab8500_regulator_pmic(dev), AB8500_VAUX3_SEL);
	if (ret < 0)
		return ret;
	ret &= AB8500_VAUX3_SEL_MASK;

	return ab8500_vaux3_voltages[ret];
}

static int ab8500_regulator_set_value(struct udevice *dev, int uV)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ab8500_vaux3_voltages); i++) {
		if (ab8500_vaux3_voltages[i] == uV)
			return pmic_clrsetbits(ab8500_regulator_pmic(dev),
					       AB8500_VAUX3_SEL,
					       AB8500_VAUX3_SEL_MASK, i);
	}

	return -EINVAL;
}

static int ab8500_regulator_get_enable(struct udevice *dev)
{
	int ret;

	ret = pmic_reg_read(ab8500_regulator_pmic(dev), AB8500_VAUX3_REGU);
	if (ret < 0)
		return ret;

	return (ret & AB8500_VAUX3_EN_MASK) == AB8500_VAUX3_EN;
}

static int ab8500_regulator_set_enable(struct udevice *dev, bool enable)
{
	return pmic_clrsetbits(ab8500_regulator_pmic(dev), AB8500_VAUX3_REGU,
			       AB8500_VAUX3_EN_MASK,
			       enable ? AB8500_VAUX3_EN : 0);
}

static const struct dm_regulator_ops ab8500_regulator_ops = {
	.get_value = ab8500_regulator_get_value,
	.set_value = ab8500_regulator_set_value,
	.get_enable = ab8500_regulator_get_enable,
	.set_enable = ab8500_regulator_set_enable,
};

U_BOOT_DRIVER(ab8500_ldo_aux3) = {
	.name = "ab8500_ldo_aux3",
	.id = UCLASS_REGULATOR,
	.ops = &ab8500_regulator_ops,
};

static int ab8500_regulators_bind(struct udevice *dev)
{
	struct driver *drv = lists_driver_lookup_name("ab8500_ldo_aux3");
	ofnode node;

	dev_for_each_subnode(node, dev) {
		if (!strcmp(ofnode_get_name(node), "ab8500_ldo_aux3"))
			return device_bind_with_driver_data(dev, drv,
						    ofnode_get_name(node), 0,
						    node, NULL);
	}

	return -ENODEV;
}

static const struct udevice_id ab8500_regulator_ids[] = {
	{ .compatible = "stericsson,ab8500-regulator" },
	{ .compatible = "stericsson,ab8505-regulator" },
	{ }
};

U_BOOT_DRIVER(ab8500_regulators) = {
	.name = "ab8500_regulators",
	.id = UCLASS_NOP,
	.of_match = ab8500_regulator_ids,
	.bind = ab8500_regulators_bind,
};
