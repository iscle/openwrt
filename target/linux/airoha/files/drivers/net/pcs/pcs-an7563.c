// SPDX-License-Identifier: GPL-2.0-only
/* AN7563 PON SerDes operating as a fixed-rate 2500BASE-X Ethernet PCS. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pcs/pcs-provider.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SCU_WAN_CONF 0x70
#define SCU_WAN_MODE GENMASK(7, 0)
#define SCU_SERDES_SEL 0x9c
#define SCU_PON_SEL GENMASK(10, 9)
#define PCS_AN_BMCR 0x0000
#define PCS_AN_BMSR 0x0004
#define PCS_CONTROL 0x0a00
#define PCS_TBI_10BIT BIT(30)
#define PCS_FORCE_MODE 0x0a24
#define PCS_FORCE_MODE_MASK (BIT(0) | GENMASK(5, 4))
#define PCS_LINK_MODE 0x4018
#define PCS_LINK_MODE_MASK (BIT(2) | GENMASK(5, 4))
#define PCS_RATE_CONTROL 0x6000
#define PCS_RATE_ENABLE (BIT(0) | BIT(4))
#define PCS_RATE_BYPASS (BIT(26) | BIT(27))
#define PCS_RATE_FORCE 0x602c
#define PCS_RATE_FORCE_MASK (BIT(8) | GENMASK(15, 12))
#define PCS_RATE_PORT_CONTROL 0x6100
#define PCS_DISABLE_MII BIT(31)

struct an7563_pcs {
	struct phylink_pcs pcs;
	struct regmap *scu;
	void __iomem *ana;
	void __iomem *mac;
};

/* Masked analog/PMA settings retain the remaining reset and calibration bits.
 * Enable the HSGMII clocks and resets, select 2.5G oversampling, then latch
 * the LCPLL frequency before enabling automatic analog initialization.
 */
static const struct an7563_pma_setting {
	u16 reg;
	u32 mask;
	u32 value;
} an7563_pma_settings[] = {
	{ 0x1500, 0x00001000, 0x00000000 },
	{ 0x1330, 0x0000001f, 0x0000001e },
	{ 0x1338, 0x0000001f, 0x0000001e },
	{ 0x1328, 0x0000001f, 0x0000001e },
	{ 0x1324, 0x00f0001f, 0x00f00010 },
	{ 0x0024, 0x00001000, 0x00000000 },
	{ 0x0018, 0x000000ff, 0x00000000 },
	{ 0x0010, 0x000ff000, 0x00010000 },
	{ 0x1600, 0x07f00000, 0x07900000 },
	{ 0x164c, 0x0000000f, 0x00000003 },
	{ 0x1604, 0x00000030, 0x00000010 },
	{ 0x1610, 0x000001ff, 0x0000003e },
	{ 0x1228, 0x00000100, 0x00000000 },
	{ 0x1364, 0x00000c00, 0x00000000 },
	{ 0x136c, 0x0fff0fff, 0x06180618 },
	{ 0x0024, 0x00001000, 0x00000000 },
	{ 0x1300, 0x370003f0, 0x240003c0 },
	{ 0x1310, 0x0000003f, 0x0000003f },
	{ 0x1608, 0x02000000, 0x02000000 },
	{ 0x1530, 0xffff0000, 0x00380000 },
	{ 0x0024, 0x08000000, 0x00000000 },
	{ 0x0008, 0x00400000, 0x00400000 },
	{ 0x1320, 0x00000013, 0x00000011 },
	{ 0x1314, 0x00000001, 0x00000001 },
	{ 0x1600, 0x00003000, 0x00000000 },
	{ 0x1130, 0x7fffffff, 0x7d000000 },
	{ 0x113c, 0x01000000, 0x00000000 },
	{ 0x1134, 0x00000100, 0x00000100 },
	{ 0x113c, 0x01000000, 0x01000000 },
	{ 0x0014, 0x01000000, 0x01000000 },
	{ 0x1200, 0x00000001, 0x00000001 },
};

static void an7563_update(void __iomem *base, u32 reg, u32 mask, u32 value)
{
	writel((readl(base + reg) & ~mask) | value, base + reg);
	readl(base + reg);
}

static unsigned int an7563_inband_caps(struct phylink_pcs *pcs,
				      phy_interface_t interface)
{
	return LINK_INBAND_DISABLE;
}

static int an7563_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			phy_interface_t interface, const unsigned long *advertising,
			bool permit_pause_to_mac)
{
	struct an7563_pcs *priv = container_of(pcs, struct an7563_pcs, pcs);
	unsigned int i;
	int ret;

	if (interface != PHY_INTERFACE_MODE_2500BASEX)
		return -EINVAL;

	ret = regmap_update_bits(priv->scu, SCU_WAN_CONF, SCU_WAN_MODE, 0x11);
	if (ret)
		return ret;
	ret = regmap_update_bits(priv->scu, SCU_SERDES_SEL, SCU_PON_SEL,
				 FIELD_PREP(SCU_PON_SEL, 2));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(an7563_pma_settings); i++) {
		const struct an7563_pma_setting *s = &an7563_pma_settings[i];

		an7563_update(priv->ana, s->reg, s->mask, s->value);
		udelay(10);
	}

	an7563_update(priv->mac, PCS_CONTROL, PCS_TBI_10BIT, 0);
	an7563_update(priv->mac, PCS_FORCE_MODE, PCS_FORCE_MODE_MASK, 0);
	an7563_update(priv->mac, PCS_LINK_MODE, PCS_LINK_MODE_MASK,
			PCS_LINK_MODE_MASK);
	an7563_update(priv->mac, PCS_RATE_FORCE, PCS_RATE_FORCE_MASK, 0);
	an7563_update(priv->mac, PCS_AN_BMCR, BMCR_ANENABLE, 0);
	an7563_update(priv->mac, PCS_RATE_PORT_CONTROL, PCS_DISABLE_MII, 0);
	an7563_update(priv->mac, PCS_RATE_CONTROL,
			PCS_RATE_ENABLE | PCS_RATE_BYPASS,
			PCS_RATE_ENABLE | PCS_RATE_BYPASS);
	return 0;
}

static void an7563_get_state(struct phylink_pcs *pcs, unsigned int neg_mode,
			    struct phylink_link_state *state)
{
	struct an7563_pcs *priv = container_of(pcs, struct an7563_pcs, pcs);
	u32 bmsr;

	/* Read twice to clear the latched-low link status. */
	readl(priv->mac + PCS_AN_BMSR);
	bmsr = readl(priv->mac + PCS_AN_BMSR);
	state->link = !!(bmsr & BMSR_LSTATUS);
	state->speed = SPEED_2500;
	state->duplex = DUPLEX_FULL;
}

static const struct phylink_pcs_ops an7563_pcs_ops = {
	.pcs_inband_caps = an7563_inband_caps,
	.pcs_config = an7563_config,
	.pcs_get_state = an7563_get_state,
};

static struct phylink_pcs *an7563_pcs_get(struct fwnode_reference_args *args,
					void *data)
{
	struct an7563_pcs *priv = data;

	if (args->nargs)
		return ERR_PTR(-EINVAL);
	return &priv->pcs;
}

static int an7563_pcs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_pcs_provider *provider;
	struct an7563_pcs *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->ana = devm_platform_ioremap_resource_byname(pdev, "ana");
	if (IS_ERR(priv->ana))
		return PTR_ERR(priv->ana);
	priv->mac = devm_platform_ioremap_resource_byname(pdev, "mac");
	if (IS_ERR(priv->mac))
		return PTR_ERR(priv->mac);
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu), "SCU unavailable\n");

	priv->pcs.ops = &an7563_pcs_ops;
	priv->pcs.poll = true;
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, priv->pcs.supported_interfaces);
	provider = devm_fwnode_pcs_add_provider(dev, dev_fwnode(dev),
					       an7563_pcs_get, priv);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id an7563_pcs_match[] = {
	{ .compatible = "airoha,an7563-pcs" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7563_pcs_match);

static struct platform_driver an7563_pcs_driver = {
	.probe = an7563_pcs_probe,
	.driver = {
		.name = "an7563-pcs",
		.of_match_table = an7563_pcs_match,
	},
};
module_platform_driver(an7563_pcs_driver);
MODULE_DESCRIPTION("Airoha AN7563 2500BASE-X PCS");
MODULE_LICENSE("GPL");
