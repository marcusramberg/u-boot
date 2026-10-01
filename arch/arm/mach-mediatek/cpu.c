// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 MediaTek Inc.
 */

#include <cpu_func.h>
#include <dm.h>
#include <init.h>
#include <wdt.h>
#include <dm/uclass-internal.h>

void mt6768_trace(const char *msg);
void mt6768_stage(int n);

int arch_cpu_init(void)
{
	mt6768_stage(0);
	mt6768_trace("[UBOOT] arch_cpu_init\n");
	icache_enable();

	return 0;
}

void enable_caches(void)
{
	/*
	 * dcache_enable() does not return on MT6768: U-Boot died right after the
	 * "DRAM:" line, and initr_caches() runs before board_init() in
	 * init_sequence_r, which made it look like a DRAM problem. Skipping the
	 * MMU/D-cache enable at runtime gets U-Boot to the prompt.
	 *
	 * Do NOT use CONFIG_SYS_DCACHE_OFF for this: that config drops
	 * cache_v8.c from the build and the link then fails on a pile of missing
	 * cache functions (and made the build itself SIGSEGV, "Error 139").
	 *
	 * TODO: find out why. Running with D-cache off is slow and is not a fix;
	 * the call this replaces was dcache_enable().
	 */
	mt6768_trace("[UBOOT] enable_caches: skipping dcache_enable\n");
}
