// SPDX-License-Identifier: GPL-2.0-only
/* AN7563 switch MDIO controller. */
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/platform_device.h>

#define MDIO_BUSY BIT(31)
#define MDIO_REG GENMASK(29, 25)
#define MDIO_PHY GENMASK(24, 20)
#define MDIO_CMD GENMASK(19, 18)
#define MDIO_C22 BIT(16)
#define MDIO_DATA GENMASK(15, 0)

static int an7563_mdio_xfer(struct mii_bus *bus, int addr, int reg,
			  u16 data, bool write)
{
	void __iomem *base = bus->priv;
	u32 val;
	int ret;

	ret = readl_poll_timeout(base, val, !(val & MDIO_BUSY), 10, 100000);
	if (ret)
		return ret;

	val = MDIO_BUSY | MDIO_C22 | FIELD_PREP(MDIO_PHY, addr) |
	      FIELD_PREP(MDIO_REG, reg) | FIELD_PREP(MDIO_CMD, write ? 1 : 2);
	if (write)
		val |= data;
	writel(val, base);

	ret = readl_poll_timeout(base, val, !(val & MDIO_BUSY), 10, 100000);
	return ret ? ret : write ? 0 : FIELD_GET(MDIO_DATA, val);
}

static int an7563_mdio_read(struct mii_bus *bus, int addr, int reg)
{
	return an7563_mdio_xfer(bus, addr, reg, 0, false);
}

static int an7563_mdio_write(struct mii_bus *bus, int addr, int reg, u16 val)
{
	return an7563_mdio_xfer(bus, addr, reg, val, true);
}

static int an7563_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mii_bus *bus;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	bus = devm_mdiobus_alloc(dev);
	if (!bus)
		return -ENOMEM;
	bus->name = "AN7563 MDIO";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	bus->parent = dev;
	bus->priv = base;
	bus->read = an7563_mdio_read;
	bus->write = an7563_mdio_write;
	return devm_of_mdiobus_register(dev, bus, dev->of_node);
}

static const struct of_device_id an7563_mdio_match[] = {
	{ .compatible = "airoha,an7563-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7563_mdio_match);

static struct platform_driver an7563_mdio_driver = {
	.probe = an7563_mdio_probe,
	.driver = {
		.name = "an7563-mdio",
		.of_match_table = an7563_mdio_match,
	},
};
module_platform_driver(an7563_mdio_driver);
MODULE_DESCRIPTION("Airoha AN7563 MDIO controller");
MODULE_LICENSE("GPL");
