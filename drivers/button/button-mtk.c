// SPDX-License-Identifier: GPL-2.0+
/*
 * MediaTek phone keys: the SoC keypad and the MT6358 PMIC keys. Both are
 * read as lk left them configured; nothing is initialised here.
 */

#include <button.h>
#include <dm.h>
#include <asm/io.h>
#include <dm/lists.h>
#include <linux/bitops.h>
#include <power/pmic.h>

#define KP_MEM1			0x04
#define MT6358_TOPSTATUS	0x28

struct mtk_key_priv {
	struct udevice *pmic;	/* NULL for keypad keys */
	void __iomem *kp_mem;
	u32 mask;
};

static enum button_state_t mtk_key_get_state(struct udevice *dev)
{
	struct mtk_key_priv *priv = dev_get_priv(dev);
	u16 val;
	int ret;

	if (priv->pmic) {
		ret = pmic_read(priv->pmic, MT6358_TOPSTATUS, (u8 *)&val, 2);
		if (ret)
			return ret;
	} else {
		val = readl(priv->kp_mem);
	}

	/* both report a pressed key as 0 */
	return val & priv->mask ? BUTTON_OFF : BUTTON_ON;
}

static int mtk_key_get_code(struct udevice *dev)
{
	return dev_read_u32_default(dev, "linux,code", 0);
}

static int mtk_key_probe(struct udevice *dev)
{
	struct mtk_key_priv *priv = dev_get_priv(dev);
	struct udevice *parent = dev_get_parent(dev);

	priv->mask = BIT(dev_read_u32_default(dev, "mediatek,status-bit", 0));
	if (device_is_compatible(parent, "mediatek,mt6358-keys")) {
		priv->pmic = dev_get_parent(parent);
		return 0;
	}

	priv->kp_mem = dev_read_addr_ptr(parent);
	if (!priv->kp_mem)
		return -EINVAL;
	priv->kp_mem += KP_MEM1;

	return 0;
}

static int mtk_keys_bind(struct udevice *parent)
{
	struct button_uc_plat *uc_plat;
	struct udevice *dev;
	ofnode node;
	int ret;

	dev_for_each_subnode(node, parent) {
		ret = device_bind_driver_to_node(parent, "mtk_key",
						 ofnode_get_name(node), node,
						 &dev);
		if (ret)
			return ret;
		uc_plat = dev_get_uclass_plat(dev);
		uc_plat->label = ofnode_read_string(node, "label");
	}

	return 0;
}

static const struct button_ops mtk_key_ops = {
	.get_state = mtk_key_get_state,
	.get_code = mtk_key_get_code,
};

U_BOOT_DRIVER(mtk_key) = {
	.name = "mtk_key",
	.id = UCLASS_BUTTON,
	.ops = &mtk_key_ops,
	.probe = mtk_key_probe,
	.priv_auto = sizeof(struct mtk_key_priv),
};

static const struct udevice_id mtk_keys_ids[] = {
	{ .compatible = "mediatek,mt6358-keys" },
	{ .compatible = "mediatek,mt6768-keypad" },
	{ }
};

U_BOOT_DRIVER(mtk_keys) = {
	.name = "mtk_keys",
	.id = UCLASS_BUTTON,
	.of_match = mtk_keys_ids,
	.bind = mtk_keys_bind,
};
