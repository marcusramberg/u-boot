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

int board_early_init_f(void)
{
	/* lk leaves the watchdog armed; WDT_MODE key without the enable bit */
	if (IS_ENABLED(CONFIG_MOTOROLA_LAMUC))
		writel(0x22000000, 0x10007000);

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
