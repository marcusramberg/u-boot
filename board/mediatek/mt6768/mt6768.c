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

#include <command.h>
#include <cpu_func.h>
#include <init.h>
#include <vsprintf.h>
#include <dm/root.h>
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

/* Start an arm64 Image the way lk does, so a new U-Boot sees lk's FDT in x0. */
static int do_chainload(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	void (*entry)(ulong x0, ulong x1, ulong x2, ulong x3);
	ulong addr;

	if (argc != 2)
		return CMD_RET_USAGE;

	addr = hextoul(argv[1], NULL);
	if ((addr & 0xfff) || readl(addr + 0x38) != 0x644d5241) {
		printf("no 4K aligned arm64 Image at %lx\n", addr);
		return CMD_RET_FAILURE;
	}

	printf("## chainloading %lx, x0=%llx\n", addr,
	       (u64)get_prev_bl_fdt_addr());
	dm_remove_devices_active();
	cleanup_before_linux();
	entry = (void *)addr;
	entry(get_prev_bl_fdt_addr(), 0, 0, 0);

	return CMD_RET_FAILURE;
}

U_BOOT_CMD(chainload, 2, 0, do_chainload,
	   "start an arm64 Image with lk's FDT in x0", "<addr>");
