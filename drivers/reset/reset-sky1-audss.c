// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cix Sky1 Audio Subsystem reset controller driver
 *
 * Copyright 2026 Cix Technology Group Co., Ltd.
 */

#include <dt-bindings/reset/cix,sky1-audss-cru.h>

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define SKY1_RESET_SLEEP_US			50

#define AUDSS_SW_RST			0x78

struct sky1_audss_reset_map {
	unsigned int offset;
	unsigned int mask;
};

struct sky1_audss_reset {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	const struct sky1_audss_reset_map *map;
};

static const struct sky1_audss_reset_map sky1_audss_reset_map[] = {
	[AUDSS_I2S0_SW_RST]   = { AUDSS_SW_RST, BIT(0) },
	[AUDSS_I2S1_SW_RST]   = { AUDSS_SW_RST, BIT(1) },
	[AUDSS_I2S2_SW_RST]   = { AUDSS_SW_RST, BIT(2) },
	[AUDSS_I2S3_SW_RST]   = { AUDSS_SW_RST, BIT(3) },
	[AUDSS_I2S4_SW_RST]   = { AUDSS_SW_RST, BIT(4) },
	[AUDSS_I2S5_SW_RST]   = { AUDSS_SW_RST, BIT(5) },
	[AUDSS_I2S6_SW_RST]   = { AUDSS_SW_RST, BIT(6) },
	[AUDSS_I2S7_SW_RST]   = { AUDSS_SW_RST, BIT(7) },
	[AUDSS_I2S8_SW_RST]   = { AUDSS_SW_RST, BIT(8) },
	[AUDSS_I2S9_SW_RST]   = { AUDSS_SW_RST, BIT(9) },
	[AUDSS_WDT_SW_RST]    = { AUDSS_SW_RST, BIT(10) },
	[AUDSS_TIMER_SW_RST]  = { AUDSS_SW_RST, BIT(11) },
	[AUDSS_MB0_SW_RST]    = { AUDSS_SW_RST, BIT(12) },
	[AUDSS_MB1_SW_RST]    = { AUDSS_SW_RST, BIT(13) },
	[AUDSS_HDA_SW_RST]    = { AUDSS_SW_RST, BIT(14) },
	[AUDSS_DMAC_SW_RST]   = { AUDSS_SW_RST, BIT(15) },
};

static struct sky1_audss_reset *to_sky1_audss_reset(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct sky1_audss_reset, rcdev);
}

static int sky1_audss_reset_set(struct reset_controller_dev *rcdev,
				unsigned long id, bool assert)
{
	struct sky1_audss_reset *priv = to_sky1_audss_reset(rcdev);
	const struct sky1_audss_reset_map *signal = &priv->map[id];
	int ret;

	if (!priv->regmap)
		return -ENODEV;

	ret = regmap_assign_bits(priv->regmap, signal->offset,
				 signal->mask, !assert);
	if (ret)
		return ret;

	fsleep(SKY1_RESET_SLEEP_US);
	return 0;
}

static int sky1_audss_reset_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	return sky1_audss_reset_set(rcdev, id, true);
}

static int sky1_audss_reset_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	return sky1_audss_reset_set(rcdev, id, false);
}

static const struct reset_control_ops sky1_audss_reset_ops = {
	.assert   = sky1_audss_reset_assert,
	.deassert = sky1_audss_reset_deassert,
};

/*
 * 16th forward-port fix. CIXH6062 (the audss reset domain, sibling of
 * CIXH6061 under parent CIXH6060) is a REAL ACPI device on this hardware
 * -- confirmed via /sys/devices/platform/CIXH6060:00/CIXH6062:00 -- not
 * something created dynamically via auxiliary_bus. The 7.3-rc2 forward-
 * evolved refactor of this driver assumed an auxiliary-bus-only design
 * (matched only by name "clk_sky1_audss.reset", a child device the
 * clock driver would spawn itself), which can never bind to CIXH6062
 * because nothing on this ACPI topology ever creates that aux device.
 * The 7.2.5 reference driver binds CIXH6062 directly as a standalone
 * platform_driver via of_match_table/acpi_match_table, getting its own
 * regmap via the same syscon lookup pattern as clk-sky1-audss.c. Ported
 * that binding mechanism here, keeping this driver's more granular
 * 16-entry reset map (7.2.5 uses a single generic bit-shift instead).
 */
static int sky1_audss_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sky1_audss_reset *priv;
	struct regmap *regmap;

	if (dev->of_node) {
		struct device_node *parent_np = of_get_parent(dev->of_node);

		regmap = syscon_node_to_regmap(parent_np);
		of_node_put(parent_np);
	} else {
		regmap = ERR_PTR(-ENODEV);
	}

	if (IS_ERR(regmap))
		regmap = device_syscon_regmap_lookup_by_property(dev, "audss_cru");

	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get parent regmap\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = regmap;
	priv->map = sky1_audss_reset_map;
	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.nr_resets = ARRAY_SIZE(sky1_audss_reset_map);
	priv->rcdev.ops = &sky1_audss_reset_ops;
	priv->rcdev.of_node = dev->of_node;
	priv->rcdev.dev = dev;

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct of_device_id sky1_audss_reset_of_match[] = {
	{ .compatible = "cix,sky1-audss-reset" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sky1_audss_reset_of_match);

static const struct acpi_device_id sky1_audss_reset_acpi_match[] = {
	{ "CIXH6062", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, sky1_audss_reset_acpi_match);

static struct platform_driver sky1_audss_reset_driver = {
	.probe = sky1_audss_reset_probe,
	.driver = {
		.name = "sky1-audss-reset",
		.of_match_table = sky1_audss_reset_of_match,
		.acpi_match_table = ACPI_PTR(sky1_audss_reset_acpi_match),
	},
};
module_platform_driver(sky1_audss_reset_driver);

MODULE_AUTHOR("Joakim Zhang <joakim.zhang@cixtech.com>");
MODULE_DESCRIPTION("Cix Sky1 Audio Subsystem reset driver");
MODULE_LICENSE("GPL");
