// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synopsys eUSB2 high-speed PHY on Google Tensor zuma/zumapro, the UTMI side
 * of the USB31DRD link. Power-up as in Linux's phy-snps-eusb2.c (exynos path,
 * databook timing, zumapro cp-bias).
 */

#include <clk.h>
#include <dm.h>
#include <generic-phy.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#define HS_PHY_CTRL_RST			0x00
#define USB_PHY_RESET			BIT(0)
#define USB_PHY_RST_MASK		GENMASK(1, 0)
#define UTMI_PORT_RST_MASK		GENMASK(5, 4)

#define HS_PHY_CTRL_COMMON		0x04
#define PHY_ENABLE			BIT(0)
#define FSEL_MASK			GENMASK(6, 4)
#define RPTR_MODE			BIT(10)

#define CFG_PLLCFG0			0x08
#define PLL_CPBIAS_CNTRL_MASK		GENMASK(6, 0)
#define PLL_FB_DIV_19_8_MASK		GENMASK(19, 8)

#define CFG_PLLCFG1			0x0c
#define PLL_FB_DIV_11_8_MASK		GENMASK(11, 8)

#define CFG_TX				0x14
#define TX_FSLS_VREF_TUNE_MASK		GENMASK(2, 1)

/* 19.2 MHz reference: FSEL 0, FB_DIV 0x170 */
#define FSEL_19_2_MHZ			0x0
#define FB_DIV_19_2_MHZ			0x170

struct zuma_eusb2_phy {
	void __iomem *base;
	struct clk_bulk clks;
};

static void zuma_eusb2_rmw(struct zuma_eusb2_phy *priv, u32 off, u32 mask,
			   u32 val)
{
	clrsetbits_le32(priv->base + off, mask, val & mask);
}

static int zuma_eusb2_phy_init(struct phy *phy)
{
	struct zuma_eusb2_phy *priv = dev_get_priv(phy->dev);
	int ret;

	ret = clk_enable_bulk(&priv->clks);
	if (ret)
		return ret;

	zuma_eusb2_rmw(priv, HS_PHY_CTRL_RST, USB_PHY_RST_MASK | UTMI_PORT_RST_MASK,
		       USB_PHY_RST_MASK | UTMI_PORT_RST_MASK);
	udelay(50);

	zuma_eusb2_rmw(priv, HS_PHY_CTRL_COMMON, RPTR_MODE, RPTR_MODE);

	/*
	 * clk-zuma models the reference as an undivided 614.4 MHz mux; the pin
	 * gets 19.2 MHz, which is what Linux falls back to as well.
	 */
	zuma_eusb2_rmw(priv, HS_PHY_CTRL_COMMON, FSEL_MASK,
		       FIELD_PREP(FSEL_MASK, FSEL_19_2_MHZ));
	zuma_eusb2_rmw(priv, CFG_PLLCFG0, PLL_FB_DIV_19_8_MASK,
		       FIELD_PREP(PLL_FB_DIV_19_8_MASK, FB_DIV_19_2_MHZ));
	zuma_eusb2_rmw(priv, CFG_PLLCFG1, PLL_FB_DIV_11_8_MASK, 0);
	zuma_eusb2_rmw(priv, CFG_PLLCFG0, PLL_CPBIAS_CNTRL_MASK, 0);
	zuma_eusb2_rmw(priv, CFG_TX, TX_FSLS_VREF_TUNE_MASK, 0);

	/*
	 * UTMI_TESTSE (+0x20) is not writable here -- the access aborts -- so
	 * TEST_IDDQ stays as ABL left it (clear).
	 */

	/* Databook power-up; the external repeater must latch Port-Reset. */
	zuma_eusb2_rmw(priv, HS_PHY_CTRL_COMMON, PHY_ENABLE, 0);
	udelay(10);
	zuma_eusb2_rmw(priv, HS_PHY_CTRL_RST, USB_PHY_RESET, 0);
	udelay(10);
	zuma_eusb2_rmw(priv, HS_PHY_CTRL_COMMON, PHY_ENABLE, PHY_ENABLE);
	udelay(1000 + 28 + 2500);

	zuma_eusb2_rmw(priv, HS_PHY_CTRL_RST, UTMI_PORT_RST_MASK, 0);

	return 0;
}

static int zuma_eusb2_phy_exit(struct phy *phy)
{
	struct zuma_eusb2_phy *priv = dev_get_priv(phy->dev);

	return clk_disable_bulk(&priv->clks);
}

static int zuma_eusb2_phy_probe(struct udevice *dev)
{
	struct zuma_eusb2_phy *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	return clk_get_bulk(dev, &priv->clks);
}

static const struct phy_ops zuma_eusb2_phy_ops = {
	.init	= zuma_eusb2_phy_init,
	.exit	= zuma_eusb2_phy_exit,
};

static const struct udevice_id zuma_eusb2_phy_of_match[] = {
	{ .compatible = "google,zuma-eusb2-phy" },
	{ }
};

U_BOOT_DRIVER(zuma_eusb2_phy) = {
	.name		= "zuma-eusb2-phy",
	.id		= UCLASS_PHY,
	.of_match	= zuma_eusb2_phy_of_match,
	.probe		= zuma_eusb2_phy_probe,
	.ops		= &zuma_eusb2_phy_ops,
	.priv_auto	= sizeof(struct zuma_eusb2_phy),
};
