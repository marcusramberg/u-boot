// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal Samsung Exynos / Google Tensor UFS platform binding.
 */

#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <regmap.h>
#include <syscon.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>

#include "ufs.h"

/* Exynos vendor HCI registers */
#define HCI_TXPRDT_ENTRY_SIZE			0x00
#define PRDT_PREFETCH_EN			BIT(31)
#define HCI_RXPRDT_ENTRY_SIZE			0x04
#define HCI_UTRL_NEXUS_TYPE			0x40
#define HCI_UTMRL_NEXUS_TYPE			0x44
#define HCI_SW_RST				0x50
#define UFS_LINK_SW_RST				BIT(0)
#define UFS_UNIPRO_SW_RST			BIT(1)
#define UFS_SW_RST_MASK				(UFS_UNIPRO_SW_RST | UFS_LINK_SW_RST)
#define HCI_DATA_REORDER			0x60
#define HCI_UNIPRO_APB_CLK_CTRL			0x68
#define UNIPRO_APB_CLK(v, x)			(((v) & ~0xf) | ((x) & 0xf))
#define HCI_AXIDMA_RWDATA_BURST_LEN		0x6c
#define WLU_EN					BIT(31)
#define WLU_BURST_LEN(x)			(((x) << 27) | ((x) & 0xf))
#define HCI_GPIO_OUT				0x70
#define HCI_V2P1_CTRL				0x8c
#define IA_TICK_SEL				BIT(16)
#define HCI_CLKSTOP_CTRL			0xb0
#define REFCLKOUT_STOP				BIT(4)
#define MPHY_APBCLK_STOP			BIT(3)
#define REFCLK_STOP				BIT(2)
#define UNIPRO_MCLK_STOP			BIT(1)
#define UNIPRO_PCLK_STOP			BIT(0)
#define CLK_STOP_MASK				(REFCLKOUT_STOP | MPHY_APBCLK_STOP | \
						 REFCLK_STOP | UNIPRO_MCLK_STOP | \
						 UNIPRO_PCLK_STOP)
#define HCI_MISC				0xb4
#define REFCLK_CTRL_EN				BIT(7)
#define UNIPRO_PCLK_CTRL_EN			BIT(6)
#define UNIPRO_MCLK_CTRL_EN			BIT(5)
#define HCI_CORECLK_CTRL_EN			BIT(4)
#define CLK_CTRL_EN_MASK			(REFCLK_CTRL_EN | UNIPRO_PCLK_CTRL_EN | \
						 UNIPRO_MCLK_CTRL_EN)
#define HCI_IOP_ACG_DISABLE			0x100
#define HCI_IOP_ACG_DISABLE_EN			BIT(0)

/* HSI2 sysreg IO coherency bits */
#define UFS_GS101_RD_SHARABLE			BIT(0)
#define UFS_GS101_WR_SHARABLE			BIT(1)
#define UFS_GS101_SHARABLE			(UFS_GS101_RD_SHARABLE | \
						 UFS_GS101_WR_SHARABLE)
#define UFS_SHAREABILITY_OFFSET			0x710

/* UniPro registers and GS101 MIBs from Linux ufs-exynos. */
#define COMP_CLK_PERIOD				0x44
#define UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER0	0x7888
#define UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER1	0x788c
#define UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER2	0x7890
#define UNIPRO_DME_POWERMODE_REQ_REMOTEL2TIMER0	0x78b8
#define UNIPRO_DME_POWERMODE_REQ_REMOTEL2TIMER1	0x78bc
#define UNIPRO_DME_POWERMODE_REQ_REMOTEL2TIMER2	0x78c0
#define PA_GS101_DBG_OPTION_SUITE1		0x956a
#define PA_GS101_DBG_OPTION_SUITE2		0x956d
#define VND_TX_CLK_PRD				0xaa
#define VND_TX_CLK_PRD_EN			0xa9
#define VND_TX_LINERESET_PVALUE0		0xad
#define VND_TX_LINERESET_PVALUE1		0xac
#define VND_TX_LINERESET_PVALUE2		0xab
#define TX_LINE_RESET_TIME			3200
#define VND_RX_CLK_PRD				0x12
#define VND_RX_CLK_PRD_EN			0x11
#define VND_RX_LINERESET_VALUE0			0x1d
#define VND_RX_LINERESET_VALUE1			0x1c
#define VND_RX_LINERESET_VALUE2			0x1b
#define RX_LINE_RESET_TIME			1000
#define CPORT_IDLE				0
#define CPORT_CONNECTED				1
#define CPORT_DEF_FLAGS			0x6

/* GS101 UFS PHY PMA registers. */
#define PHY_APB_ADDR(off)			((off) << 2)
#define PHY_GS101_LANE_OFFSET			0x200
#define PHY_PMA_TRSV_ADDR(reg, lane)		PHY_APB_ADDR((reg) + \
						     ((lane) * PHY_GS101_LANE_OFFSET))
#define TRSV_REG338				0x338
#define LN0_MON_RX_CAL_DONE			BIT(3)
#define TRSV_REG31D				0x31d
#define LN0_MON_RX_CAL_DONE_ZUMA		BIT(3)

#define TENSOR_GS101_PHY_CTRL			0x3ec8
#define TENSOR_GS101_PHY_CTRL_MASK		0x1
#define TENSOR_GS101_PHY_CTRL_EN		BIT(0)

#define UFS_EXYNOS_MCLK_RATE			245000000UL
#define UFS_EXYNOS_MAX_LANES			2
#define UFS_EXYNOS_PRDT_ENTRY_SIZE		12
#define UFS_EXYNOS_FMP_SG_ENTRY_SIZE		128

#define SMC_CMD_FMP_SECURITY			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
			   ARM_SMCCC_OWNER_SIP, 0x1810)
#define SMC_CMD_SMU				\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
			   ARM_SMCCC_OWNER_SIP, 0x1850)
#define SMU_EMBEDDED				0
#define SMU_INIT				0
#define CFG_DESCTYPE_3				3

struct exynos_ufs_phy_cfg {
	u32 off_0;
	u32 off_1;
	u32 val;
	bool per_lane;
};

#define PHY_COMN_REG_CFG(o, v)			{ PHY_APB_ADDR(o), 0, v, false }
#define PHY_TRSV_REG_CFG_GS101(o, v)		{ PHY_APB_ADDR(o), \
						  PHY_APB_ADDR((o) + PHY_GS101_LANE_OFFSET), \
						  v, true }
#define END_UFS_PHY_CFG				{ 0, 0, 0, false }

static const struct exynos_ufs_phy_cfg tensor_gs101_pre_init_cfg[] = {
	PHY_COMN_REG_CFG(0x43, 0x10),
	PHY_COMN_REG_CFG(0x3c, 0x14),
	PHY_COMN_REG_CFG(0x46, 0x48),
	PHY_TRSV_REG_CFG_GS101(0x200, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x201, 0x06),
	PHY_TRSV_REG_CFG_GS101(0x202, 0x06),
	PHY_TRSV_REG_CFG_GS101(0x203, 0x0a),
	PHY_TRSV_REG_CFG_GS101(0x204, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x205, 0x11),
	PHY_TRSV_REG_CFG_GS101(0x207, 0x0c),
	PHY_TRSV_REG_CFG_GS101(0x2e1, 0xc0),
	PHY_TRSV_REG_CFG_GS101(0x22d, 0xb8),
	PHY_TRSV_REG_CFG_GS101(0x234, 0x60),
	PHY_TRSV_REG_CFG_GS101(0x238, 0x13),
	PHY_TRSV_REG_CFG_GS101(0x239, 0x48),
	PHY_TRSV_REG_CFG_GS101(0x23a, 0x01),
	PHY_TRSV_REG_CFG_GS101(0x23b, 0x25),
	PHY_TRSV_REG_CFG_GS101(0x23c, 0x2a),
	PHY_TRSV_REG_CFG_GS101(0x23d, 0x01),
	PHY_TRSV_REG_CFG_GS101(0x23e, 0x13),
	PHY_TRSV_REG_CFG_GS101(0x23f, 0x13),
	PHY_TRSV_REG_CFG_GS101(0x240, 0x4a),
	PHY_TRSV_REG_CFG_GS101(0x243, 0x40),
	PHY_TRSV_REG_CFG_GS101(0x244, 0x02),
	PHY_TRSV_REG_CFG_GS101(0x25d, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x25e, 0x3f),
	PHY_TRSV_REG_CFG_GS101(0x25f, 0xff),
	PHY_TRSV_REG_CFG_GS101(0x273, 0x33),
	PHY_TRSV_REG_CFG_GS101(0x274, 0x50),
	PHY_TRSV_REG_CFG_GS101(0x284, 0x02),
	PHY_TRSV_REG_CFG_GS101(0x285, 0x02),
	PHY_TRSV_REG_CFG_GS101(0x2a2, 0x04),
	PHY_TRSV_REG_CFG_GS101(0x25d, 0x01),
	PHY_TRSV_REG_CFG_GS101(0x2fa, 0x01),
	PHY_TRSV_REG_CFG_GS101(0x286, 0x03),
	PHY_TRSV_REG_CFG_GS101(0x287, 0x03),
	PHY_TRSV_REG_CFG_GS101(0x288, 0x03),
	PHY_TRSV_REG_CFG_GS101(0x289, 0x03),
	PHY_TRSV_REG_CFG_GS101(0x2b3, 0x04),
	PHY_TRSV_REG_CFG_GS101(0x2b6, 0x0b),
	PHY_TRSV_REG_CFG_GS101(0x2b7, 0x0b),
	PHY_TRSV_REG_CFG_GS101(0x2b8, 0x0b),
	PHY_TRSV_REG_CFG_GS101(0x2b9, 0x0b),
	PHY_TRSV_REG_CFG_GS101(0x2ba, 0x0b),
	PHY_TRSV_REG_CFG_GS101(0x2bb, 0x06),
	PHY_TRSV_REG_CFG_GS101(0x2bc, 0x06),
	PHY_TRSV_REG_CFG_GS101(0x2bd, 0x06),
	PHY_TRSV_REG_CFG_GS101(0x29e, 0x06),
	PHY_TRSV_REG_CFG_GS101(0x2e4, 0x1a),
	PHY_TRSV_REG_CFG_GS101(0x2ed, 0x25),
	PHY_TRSV_REG_CFG_GS101(0x269, 0x1a),
	PHY_TRSV_REG_CFG_GS101(0x2f4, 0x2f),
	PHY_TRSV_REG_CFG_GS101(0x34b, 0x01),
	PHY_TRSV_REG_CFG_GS101(0x34c, 0x23),
	PHY_TRSV_REG_CFG_GS101(0x34d, 0x23),
	PHY_TRSV_REG_CFG_GS101(0x34e, 0x45),
	PHY_TRSV_REG_CFG_GS101(0x34f, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x350, 0x31),
	PHY_TRSV_REG_CFG_GS101(0x351, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x352, 0x02),
	PHY_TRSV_REG_CFG_GS101(0x353, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x354, 0x01),
	PHY_COMN_REG_CFG(0x43, 0x18),
	PHY_COMN_REG_CFG(0x43, 0x00),
	END_UFS_PHY_CFG,
};

static const struct exynos_ufs_phy_cfg tensor_zuma_pre_init_cfg[] = {
	PHY_COMN_REG_CFG(0x50, 0x08),
	PHY_COMN_REG_CFG(0x05, 0x19),
	PHY_COMN_REG_CFG(0x0b, 0x44),
	PHY_COMN_REG_CFG(0x0c, 0xc4),
	PHY_COMN_REG_CFG(0x0d, 0xc3),
	PHY_COMN_REG_CFG(0x0f, 0x88),
	PHY_COMN_REG_CFG(0x16, 0x1a),
	PHY_COMN_REG_CFG(0x19, 0x04),
	PHY_COMN_REG_CFG(0x54, 0x88),
	PHY_COMN_REG_CFG(0x67, 0x4c),
	PHY_COMN_REG_CFG(0x68, 0x4c),
	PHY_TRSV_REG_CFG_GS101(0x201, 0x44),
	PHY_TRSV_REG_CFG_GS101(0x202, 0x44),
	PHY_TRSV_REG_CFG_GS101(0x203, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x204, 0x18),
	PHY_TRSV_REG_CFG_GS101(0x205, 0xc0),
	PHY_TRSV_REG_CFG_GS101(0x207, 0x1c),
	PHY_TRSV_REG_CFG_GS101(0x2ec, 0x8c),
	PHY_TRSV_REG_CFG_GS101(0x27c, 0xd0),
	PHY_TRSV_REG_CFG_GS101(0x288, 0xfa),
	PHY_TRSV_REG_CFG_GS101(0x289, 0x60),
	PHY_TRSV_REG_CFG_GS101(0x234, 0x30),
	PHY_TRSV_REG_CFG_GS101(0x239, 0x05),
	PHY_TRSV_REG_CFG_GS101(0x23d, 0x05),
	PHY_TRSV_REG_CFG_GS101(0x24d, 0x1a),
	PHY_TRSV_REG_CFG_GS101(0x24e, 0x12),
	PHY_TRSV_REG_CFG_GS101(0x24f, 0x5e),
	PHY_TRSV_REG_CFG_GS101(0x259, 0x2a),
	PHY_TRSV_REG_CFG_GS101(0x260, 0x54),
	PHY_TRSV_REG_CFG_GS101(0x266, 0x54),
	PHY_TRSV_REG_CFG_GS101(0x273, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x274, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x2ab, 0x00),
	PHY_TRSV_REG_CFG_GS101(0x2ac, 0x02),
	PHY_COMN_REG_CFG(0x50, 0x0c),
	PHY_COMN_REG_CFG(0x50, 0x00),
	END_UFS_PHY_CFG,
};

static const struct exynos_ufs_phy_cfg tensor_gs101_pre_pwr_hs_cfg[] = {
	PHY_TRSV_REG_CFG_GS101(0x369, 0x11),
	PHY_TRSV_REG_CFG_GS101(0x246, 0x03),
	END_UFS_PHY_CFG,
};

static const struct exynos_ufs_phy_cfg tensor_gs101_post_pwr_hs_cfg[] = {
	PHY_COMN_REG_CFG(0x08, 0x60),
	PHY_TRSV_REG_CFG_GS101(0x222, 0x08),
	PHY_TRSV_REG_CFG_GS101(0x246, 0x01),
	END_UFS_PHY_CFG,
};

struct exynos_ufs_priv {
	void __iomem *reg_hci;
	void __iomem *reg_unipro;
	void __iomem *reg_ufsp;
	void __iomem *reg_phy;
	struct clk_bulk clks;
	struct clk phy_ref_clk;
	struct regmap *phy_pmu;
	struct regmap *sysreg;
	u32 iocc_offset;
	bool clks_enabled;
	bool phy_ref_clk_enabled;
	bool is_zuma;
};

static inline void hci_writel(struct exynos_ufs_priv *priv, u32 val, u32 reg)
{
	writel(val, priv->reg_hci + reg);
}

static inline u32 hci_readl(struct exynos_ufs_priv *priv, u32 reg)
{
	return readl(priv->reg_hci + reg);
}

static inline void unipro_writel(struct exynos_ufs_priv *priv, u32 val, u32 reg)
{
	writel(val, priv->reg_unipro + reg);
}

static void exynos_ufs_phy_apply_cfg(struct exynos_ufs_priv *priv,
				     const struct exynos_ufs_phy_cfg *cfg)
{
	for (; cfg->off_0 || cfg->off_1; cfg++) {
		writel(cfg->val, priv->reg_phy + cfg->off_0);
		if (cfg->per_lane)
			writel(cfg->val, priv->reg_phy + cfg->off_1);
	}
}

static void exynos_ufs_ctrl_clkstop(struct exynos_ufs_priv *priv, bool en)
{
	u32 ctrl = hci_readl(priv, HCI_CLKSTOP_CTRL);
	u32 misc = hci_readl(priv, HCI_MISC);

	if (en) {
		hci_writel(priv, misc | CLK_CTRL_EN_MASK, HCI_MISC);
		hci_writel(priv, ctrl | CLK_STOP_MASK, HCI_CLKSTOP_CTRL);
	} else {
		hci_writel(priv, ctrl & ~CLK_STOP_MASK, HCI_CLKSTOP_CTRL);
		hci_writel(priv, misc & ~CLK_CTRL_EN_MASK, HCI_MISC);
	}
}

static void exynos_ufs_auto_ctrl_hcc(struct exynos_ufs_priv *priv, bool en)
{
	u32 misc = hci_readl(priv, HCI_MISC);

	if (en)
		hci_writel(priv, misc | HCI_CORECLK_CTRL_EN, HCI_MISC);
	else
		hci_writel(priv, misc & ~HCI_CORECLK_CTRL_EN, HCI_MISC);
}

static void exynos_ufs_fmp_init(struct ufs_hba *hba)
{
	struct arm_smccc_res res;
	u32 caps;

	if (!device_is_compatible(hba->dev, "google,zuma-ufs") &&
	    !device_is_compatible(hba->dev, "google,gs101-ufs"))
		return;

	caps = ufshcd_readl(hba, REG_CONTROLLER_CAPABILITIES);
	if (!(caps & MASK_CRYPTO_SUPPORT))
		return;

	arm_smccc_smc(SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED, CFG_DESCTYPE_3,
		      0, 0, 0, 0, &res);
	if (res.a0) {
		dev_warn(hba->dev, "FMP_SECURITY SMC failed: %ld\n", res.a0);
		return;
	}

	ufshcd_set_sg_entry_size(hba, UFS_EXYNOS_FMP_SG_ENTRY_SIZE);

	arm_smccc_smc(SMC_CMD_SMU, SMU_INIT, SMU_EMBEDDED, 0, 0, 0, 0, 0,
		      &res);
	if (res.a0) {
		dev_warn(hba->dev, "SMU_INIT SMC failed: %ld\n", res.a0);
		return;
	}

	dev_notice(hba->dev, "FMP/SMU initialized, PRDT stride=%zu\n",
		   ufshcd_sg_entry_size(hba));
}

static int exynos_ufs_enable_clks(struct udevice *dev, struct exynos_ufs_priv *priv)
{
	int ret;

	if (priv->clks_enabled)
		return 0;

	ret = clk_get_bulk(dev, &priv->clks);
	if (ret) {
		dev_warn(dev, "failed to get UFS clocks (%d), continuing\n", ret);
		return 0;
	}

	ret = clk_enable_bulk(&priv->clks);
	if (ret) {
		dev_warn(dev, "failed to enable UFS clocks (%d), continuing\n", ret);
		clk_release_bulk(&priv->clks);
		return 0;
	}

	priv->clks_enabled = true;
	return 0;
}

static int gs101_ufs_phy_wait_cal(struct udevice *dev, struct exynos_ufs_priv *priv,
				  int lane)
{
	ulong start = get_timer(0);
	u32 off = PHY_PMA_TRSV_ADDR(TRSV_REG338, lane);

	while (!(readl(priv->reg_phy + off) & LN0_MON_RX_CAL_DONE)) {
		if (get_timer(start) > 40) {
			dev_err(dev, "timeout waiting for UFS PHY lane %d calibration\n",
				lane);
			return -ETIMEDOUT;
		}
		udelay(40);
	}

	return 0;
}

static int zuma_ufs_phy_wait_cal(struct udevice *dev, struct exynos_ufs_priv *priv,
				 int lane)
{
	ulong start = get_timer(0);
	u32 off_c74 = PHY_PMA_TRSV_ADDR(TRSV_REG31D, lane);
	u32 off_ce0 = PHY_PMA_TRSV_ADDR(TRSV_REG338, lane);
	u32 val_c74 = 0;
	u32 val_ce0 = 0;

	do {
		val_c74 = readl(priv->reg_phy + off_c74);
		val_ce0 = readl(priv->reg_phy + off_ce0);

		if ((val_c74 & LN0_MON_RX_CAL_DONE_ZUMA) ||
		    (val_ce0 & LN0_MON_RX_CAL_DONE))
			return 0;

		udelay(40);
	} while (get_timer(start) <= 200);

	dev_err(dev, "timeout waiting for Zuma UFS PHY lane %d calibration "
		"(c74=0x%x ce0=0x%x)\n", lane, val_c74, val_ce0);
	return -ETIMEDOUT;
}

static int gs101_ufs_phy_init(struct udevice *dev, struct exynos_ufs_priv *priv,
			      u32 lanes)
{
	const struct exynos_ufs_phy_cfg *cfg;
	struct ofnode_phandle_args args;
	ofnode pmu_node;
	int ret, lane;

	if (!priv->reg_phy) {
		ret = dev_read_phandle_with_args(dev, "phys", "#phy-cells", 0, 0,
						 &args);
		if (ret) {
			dev_err(dev, "failed to parse UFS PHY phandle (%d)\n", ret);
			return ret;
		}

		priv->reg_phy = (void __iomem *)ofnode_get_addr(args.node);
		if ((fdt_addr_t)priv->reg_phy == FDT_ADDR_T_NONE) {
			dev_err(dev, "failed to map UFS PHY registers\n");
			return -EINVAL;
		}

		pmu_node = ofnode_parse_phandle(args.node, "samsung,pmu-syscon", 0);
		if (ofnode_valid(pmu_node))
			priv->phy_pmu = syscon_node_to_regmap(pmu_node);

		ret = clk_get_by_name_nodev(args.node, "ref_clk",
					    &priv->phy_ref_clk);
		if (ret)
			dev_warn(dev, "failed to get UFS PHY ref_clk (%d)\n", ret);

		if (ofnode_device_is_compatible(args.node, "google,zuma-ufs-phy"))
			priv->is_zuma = true;
	}

	if (!IS_ERR_OR_NULL(priv->phy_pmu))
		regmap_update_bits(priv->phy_pmu, TENSOR_GS101_PHY_CTRL,
				   TENSOR_GS101_PHY_CTRL_MASK,
				   TENSOR_GS101_PHY_CTRL_EN);

	if (priv->phy_ref_clk.dev && !priv->phy_ref_clk_enabled) {
		ret = clk_enable(&priv->phy_ref_clk);
		if (ret)
			dev_warn(dev, "failed to enable UFS PHY ref_clk (%d)\n",
				 ret);
		else
			priv->phy_ref_clk_enabled = true;
	}

	cfg = priv->is_zuma ? tensor_zuma_pre_init_cfg : tensor_gs101_pre_init_cfg;
	exynos_ufs_phy_apply_cfg(priv, cfg);

	for (lane = 0; lane < lanes; lane++) {
		if (priv->is_zuma)
			ret = zuma_ufs_phy_wait_cal(dev, priv, lane);
		else
			ret = gs101_ufs_phy_wait_cal(dev, priv, lane);
		if (ret)
			return ret;
	}

	return 0;
}

static int exynos_ufs_init(struct ufs_hba *hba)
{
	struct udevice *dev = hba->dev;
	struct exynos_ufs_priv *priv = dev_get_priv(dev);
	ofnode sysreg_node;
	u32 reg;
	int ret;

	priv->reg_hci = dev_read_addr_name_ptr(dev, "vs_hci");
	priv->reg_unipro = dev_read_addr_name_ptr(dev, "unipro");
	priv->reg_ufsp = dev_read_addr_name_ptr(dev, "ufsp");

	if (!priv->reg_hci || !priv->reg_unipro || !priv->reg_ufsp) {
		dev_err(dev, "failed to map Exynos UFS vendor registers\n");
		return -EINVAL;
	}

	if (device_is_compatible(dev, "google,zuma-ufs"))
		priv->is_zuma = true;

	exynos_ufs_enable_clks(dev, priv);
	exynos_ufs_fmp_init(hba);

	sysreg_node = ofnode_parse_phandle(dev_ofnode(dev), "samsung,sysreg", 0);
	if (ofnode_valid(sysreg_node)) {
		priv->sysreg = syscon_node_to_regmap(sysreg_node);
		ret = dev_read_u32_index(dev, "samsung,sysreg", 1,
					 &priv->iocc_offset);
		if (ret)
			priv->iocc_offset = UFS_SHAREABILITY_OFFSET;

		if (!IS_ERR_OR_NULL(priv->sysreg)) {
			ret = regmap_update_bits(priv->sysreg, priv->iocc_offset,
						 UFS_GS101_SHARABLE,
						 dev_read_bool(dev, "dma-coherent") ?
						 UFS_GS101_SHARABLE : 0);
			if (ret)
				dev_warn(dev, "failed to configure UFS IO coherency (%d)\n",
					 ret);
		}
	}

	if (priv->is_zuma) {
		hba->quirks |= UFSHCD_QUIRK_BROKEN_64BIT_ADDRESS |
			       UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL |
			       UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING;
	} else {
		hba->quirks |= UFSHCD_QUIRK_BROKEN_64BIT_ADDRESS |
			       UFSHCD_QUIRK_BROKEN_LCC |
			       UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR |
			       UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR |
			       UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR |
			       UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL |
			       UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING;
	}

	reg = hci_readl(priv, HCI_IOP_ACG_DISABLE);
	hci_writel(priv, reg & ~HCI_IOP_ACG_DISABLE_EN, HCI_IOP_ACG_DISABLE);

	return 0;
}

static void exynos_ufs_establish_connt(struct ufs_hba *hba)
{
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), CPORT_IDLE);
	ufshcd_dme_set(hba, UIC_ARG_MIB(N_DEVICEID), 0);
	ufshcd_dme_set(hba, UIC_ARG_MIB(N_DEVICEID_VALID), 1);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_PEERDEVICEID), 1);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_PEERCPORTID), 0);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_CPORTFLAGS), CPORT_DEF_FLAGS);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_TRAFFICCLASS), 0);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), CPORT_CONNECTED);
}

static int exynos_ufs_hce_enable_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	ulong start;
	u32 saved_misc;

	if (status != PRE_CHANGE)
		return 0;

	saved_misc = hci_readl(priv, HCI_MISC);
	exynos_ufs_auto_ctrl_hcc(priv, false);

	hci_writel(priv, UFS_SW_RST_MASK, HCI_SW_RST);
	start = get_timer(0);
	while (hci_readl(priv, HCI_SW_RST) & UFS_SW_RST_MASK) {
		if (get_timer(start) > 1) {
			dev_err(hba->dev, "timeout host sw-reset\n");
			hci_writel(priv, saved_misc, HCI_MISC);
			return -ETIMEDOUT;
		}
	}

	hci_writel(priv, saved_misc, HCI_MISC);
	hci_writel(priv, 0, HCI_GPIO_OUT);
	udelay(13);
	hci_writel(priv, 1, HCI_GPIO_OUT);
	mdelay(13);

	return 0;
}

static int gs101_ufs_pre_link(struct ufs_hba *hba)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	u32 tx_line_reset_period;
	u32 rx_line_reset_period;
	u32 rx_lanes = 1;
	u32 tx_lanes = 1;
	u32 clk_prd;
	int i, ret;

	exynos_ufs_enable_clks(hba->dev, priv);
	exynos_ufs_auto_ctrl_hcc(priv, false);
	exynos_ufs_ctrl_clkstop(priv, false);

	ufshcd_dme_get(hba, UIC_ARG_MIB(PA_AVAILRXDATALANES), &rx_lanes);
	ufshcd_dme_get(hba, UIC_ARG_MIB(PA_AVAILTXDATALANES), &tx_lanes);
	if (!rx_lanes || rx_lanes > UFS_EXYNOS_MAX_LANES)
		rx_lanes = 1;
	if (!tx_lanes || tx_lanes > UFS_EXYNOS_MAX_LANES)
		tx_lanes = rx_lanes;

	ret = gs101_ufs_phy_init(hba->dev, priv, rx_lanes);
	if (ret)
		return ret;

	hci_writel(priv, UNIPRO_APB_CLK(hci_readl(priv, HCI_UNIPRO_APB_CLK_CTRL), 0),
		   HCI_UNIPRO_APB_CLK_CTRL);

	unipro_writel(priv, 16 * 1000 * 1000000UL / UFS_EXYNOS_MCLK_RATE,
		      COMP_CLK_PERIOD);

	if (priv->is_zuma) {
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x44), 0x00);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x202), 0x22);
	}

	ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXTRAILINGCLOCKS), 0xff);
	ufshcd_dme_set(hba, UIC_ARG_MIB(PA_GS101_DBG_OPTION_SUITE1),
		       0x90913c1c);
	ufshcd_dme_set(hba, UIC_ARG_MIB(PA_GS101_DBG_OPTION_SUITE2),
		       0xe01c115f);

	rx_line_reset_period = RX_LINE_RESET_TIME * UFS_EXYNOS_MCLK_RATE /
			       1000000UL;
	tx_line_reset_period = TX_LINE_RESET_TIME * UFS_EXYNOS_MCLK_RATE /
			       1000000UL;
	clk_prd = DIV_ROUND_UP(1000000000UL, UFS_EXYNOS_MCLK_RATE);

	ufshcd_dme_set(hba, UIC_ARG_MIB(0x200), 0x40);

	for (i = 0; i < rx_lanes; i++) {
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_RX_CLK_PRD, i), clk_prd);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_RX_CLK_PRD_EN, i), 0);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_RX_LINERESET_VALUE2, i),
			       (rx_line_reset_period >> 16) & 0xff);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_RX_LINERESET_VALUE1, i),
			       (rx_line_reset_period >> 8) & 0xff);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_RX_LINERESET_VALUE0, i),
			       rx_line_reset_period & 0xff);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x2f, i),
			       priv->is_zuma ? 0x79 : 0x69);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x84, i), 0x1);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x25, i), 0xf6);
	}

	for (i = 0; i < tx_lanes; i++) {
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_TX_CLK_PRD, i), clk_prd);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_TX_CLK_PRD_EN, i), 0x02);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_TX_LINERESET_PVALUE2, i),
			       (tx_line_reset_period >> 16) & 0xff);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_TX_LINERESET_PVALUE1, i),
			       (tx_line_reset_period >> 8) & 0xff);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(VND_TX_LINERESET_PVALUE0, i),
			       tx_line_reset_period & 0xff);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x04, i), 1);
		ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(0x7f, i), 0);
	}

	ufshcd_dme_set(hba, UIC_ARG_MIB(0x200), 0);
	if (priv->is_zuma) {
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x155e), 0);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x3000), 0);
		ufshcd_dme_set(hba, UIC_ARG_MIB(0x3001), 1);
		return 0;
	}

	ufshcd_dme_set(hba, UIC_ARG_MIB(PA_LOCAL_TX_LCC_ENABLE), 0);
	ufshcd_dme_set(hba, UIC_ARG_MIB(N_DEVICEID), 0);
	ufshcd_dme_set(hba, UIC_ARG_MIB(N_DEVICEID_VALID), 1);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_PEERDEVICEID), 1);
	ufshcd_dme_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), CPORT_CONNECTED);
	ufshcd_dme_set(hba, UIC_ARG_MIB(0xa006), 0x8000);

	return 0;
}

static int gs101_ufs_post_link(struct ufs_hba *hba)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	u32 tx_prdt = UFS_EXYNOS_PRDT_ENTRY_SIZE;
	u32 val;

	exynos_ufs_establish_connt(hba);

	val = hci_readl(priv, HCI_V2P1_CTRL);
	hci_writel(priv, val | IA_TICK_SEL, HCI_V2P1_CTRL);
	hci_writel(priv, 0xa, HCI_DATA_REORDER);
	if (ufshcd_sg_entry_size(hba) > sizeof(struct ufshcd_sg_entry))
		tx_prdt |= PRDT_PREFETCH_EN;
	hci_writel(priv, tx_prdt, HCI_TXPRDT_ENTRY_SIZE);
	hci_writel(priv, UFS_EXYNOS_PRDT_ENTRY_SIZE, HCI_RXPRDT_ENTRY_SIZE);
	hci_writel(priv, 0xffffffff, HCI_UTRL_NEXUS_TYPE);
	hci_writel(priv, 0xffffffff, HCI_UTMRL_NEXUS_TYPE);
	hci_writel(priv, WLU_EN | WLU_BURST_LEN(3), HCI_AXIDMA_RWDATA_BURST_LEN);

	ufshcd_dme_set(hba, UIC_ARG_MIB(PA_SAVECONFIGTIME), 0x3e8);

	return 0;
}

static int exynos_ufs_link_startup_notify(struct ufs_hba *hba,
					  enum ufs_notify_change_status status)
{
	if (status == PRE_CHANGE)
		return gs101_ufs_pre_link(hba);
	if (status == POST_CHANGE)
		return gs101_ufs_post_link(hba);

	return 0;
}

static int exynos_ufs_device_reset(struct ufs_hba *hba)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);

	hci_writel(priv, 0, HCI_GPIO_OUT);
	udelay(13);
	hci_writel(priv, 1, HCI_GPIO_OUT);
	mdelay(13);

	return 0;
}

static bool exynos_ufs_is_hs_mode(const struct ufs_pa_layer_attr *pwr_mode)
{
	return pwr_mode->pwr_rx == FAST_MODE ||
	       pwr_mode->pwr_rx == FASTAUTO_MODE ||
	       pwr_mode->pwr_tx == FAST_MODE ||
	       pwr_mode->pwr_tx == FASTAUTO_MODE;
}

static int exynos_ufs_pwr_change_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status,
					struct ufs_pa_layer_attr *pwr_mode)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);

	if (!exynos_ufs_is_hs_mode(pwr_mode))
		return 0;

	if (status == PRE_CHANGE) {
		exynos_ufs_enable_clks(hba->dev, priv);
		exynos_ufs_ctrl_clkstop(priv, false);

		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA0), 12000);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA1), 32000);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA2), 16000);
		unipro_writel(priv, 8064, UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER0);
		unipro_writel(priv, 28224, UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER1);
		unipro_writel(priv, 20160, UNIPRO_DME_POWERMODE_REQ_LOCALL2TIMER2);
		unipro_writel(priv, 12000, UNIPRO_DME_POWERMODE_REQ_REMOTEL2TIMER0);
		unipro_writel(priv, 32000, UNIPRO_DME_POWERMODE_REQ_REMOTEL2TIMER1);
		unipro_writel(priv, 16000, UNIPRO_DME_POWERMODE_REQ_REMOTEL2TIMER2);

		if (priv->reg_phy)
			exynos_ufs_phy_apply_cfg(priv, tensor_gs101_pre_pwr_hs_cfg);
		dev_notice(hba->dev, "UFS HS pre power-mode calibration\n");
	} else if (status == POST_CHANGE) {
		if (priv->reg_phy)
			exynos_ufs_phy_apply_cfg(priv, tensor_gs101_post_pwr_hs_cfg);
		dev_notice(hba->dev, "UFS HS post power-mode calibration\n");
	}

	return 0;
}

static void exynos_ufs_setup_xfer_req(struct ufs_hba *hba, int tag,
				      bool is_scsi_cmd)
{
	struct exynos_ufs_priv *priv = dev_get_priv(hba->dev);
	u32 type = hci_readl(priv, HCI_UTRL_NEXUS_TYPE);

	if (is_scsi_cmd)
		type |= BIT(tag);
	else
		type &= ~BIT(tag);

	hci_writel(priv, type, HCI_UTRL_NEXUS_TYPE);
}

static struct ufs_hba_ops exynos_ufs_hba_ops = {
	.init = exynos_ufs_init,
	.pwr_change_notify = exynos_ufs_pwr_change_notify,
	.hce_enable_notify = exynos_ufs_hce_enable_notify,
	.link_startup_notify = exynos_ufs_link_startup_notify,
	.device_reset = exynos_ufs_device_reset,
	.setup_xfer_req = exynos_ufs_setup_xfer_req,
};

static int exynos_ufs_probe(struct udevice *dev)
{
	int ret;

	ret = ufshcd_probe(dev, &exynos_ufs_hba_ops);
	if (ret)
		dev_err(dev, "ufshcd_probe() failed %d\n", ret);

	return ret;
}

static const struct udevice_id exynos_ufs_ids[] = {
	{ .compatible = "google,zuma-ufs" },
	{ .compatible = "google,gs101-ufs" },
	{ .compatible = "samsung,exynos7-ufs" },
	{ .compatible = "samsung,exynosautov9-ufs" },
	{ .compatible = "samsung,exynosautov920-ufs" },
	{ }
};

U_BOOT_DRIVER(exynos_ufs) = {
	.name = "exynos-ufs",
	.id = UCLASS_UFS,
	.of_match = exynos_ufs_ids,
	.probe = exynos_ufs_probe,
	.priv_auto = sizeof(struct exynos_ufs_priv),
};
