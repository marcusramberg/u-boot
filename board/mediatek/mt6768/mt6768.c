// SPDX-License-Identifier: GPL-2.0
/*
 * Board support for MediaTek MT6768-based devices.
 *
 * board_init() lives in arch/arm/mach-mediatek/mt6768/init.c.
 *
 * This file adds early boot tracing into the ramoops console buffer, so that
 * progress can be recovered from Linux (/sys/fs/pstore/console-ramoops) after
 * a reboot. There is no accessible UART on these handsets.
 */

#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/io.h>

/* Console zone of the pstore reservation on Xiaomi lancelot. */
#define RAMOOPS_CONSOLE_BASE	0x4d05f000UL
#define RAMOOPS_CONSOLE_SIZE	0x40000UL
#define PERSISTENT_RAM_SIG	0x43474244	/* DBGC */

struct pram_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8  data[0];
};

static void uboot_trace(const char *msg)
{
	volatile struct pram_buffer *b =
		(volatile struct pram_buffer *)RAMOOPS_CONSOLE_BASE;
	u32 pos;
	const char *p;

	/* the console zone is only verified on lancelot */
	if (!IS_ENABLED(CONFIG_XIAOMI_LANCELOT))
		return;

	if (b->sig != PERSISTENT_RAM_SIG) {
		b->sig = PERSISTENT_RAM_SIG;
		b->start = 0;
		b->size = 0;
	}

	pos = b->start;
	for (p = msg; *p; p++) {
		if (pos >= RAMOOPS_CONSOLE_SIZE - sizeof(struct pram_buffer))
			break;
		b->data[pos++] = *p;
	}
	b->start = pos;
	b->size = pos;
}

/*
 * Debug without a console: stage n paints bands 5..5+n (0-4 are start.S) into
 * all three framebuffer pages. Redrawing the earlier bands survives the video
 * driver clearing the screen.
 */
#define LAMUC_FB	0x76f20000UL
#define LAMUC_STRIDE	(1088 * 4)
#define LAMUC_BAND	(2400 / 16)
#define LAMUC_PAGE	(LAMUC_STRIDE * 2400UL)

void mt6768_stage(int n)
{
	static const u32 colour[] = {
		0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00,
		0xff00ffff, 0xffff00ff, 0xffffffff, 0xffff8000,
	};
	int i, pg;

	if (!IS_ENABLED(CONFIG_MOTOROLA_LAMUC))
		return;
	for (pg = 0; pg < 3; pg++) {
		for (i = 0; i <= n && i < ARRAY_SIZE(colour); i++) {
			u32 *p = (u32 *)(LAMUC_FB + LAMUC_PAGE * pg +
				(u64)LAMUC_STRIDE * LAMUC_BAND * (i + 5));
			u32 *end = p + LAMUC_STRIDE / 4 * (LAMUC_BAND - 8);

			while (p < end)
				writel(colour[i], p++);
		}
	}
}

int board_early_init_f(void)
{
	/* lk leaves the watchdog armed; WDT_MODE key without the enable bit */
	if (IS_ENABLED(CONFIG_MOTOROLA_LAMUC))
		writel(0x22000000, 0x10007000);
	mt6768_stage(1);

	uboot_trace("\n[UBOOT] board_early_init_f reached\n");
	return 0;
}

/*
 * board_late_init() lives in arch/arm/mach-mediatek/mt6768/init.c.
 * Redefining it here would cause a "multiple definition" link error.
 */
void mt6768_trace(const char *msg)
{
	uboot_trace(msg);
}
