#ifndef __MT6768_H
#define __MT6768_H

#define CFG_SYS_SDRAM_BASE 0x40000000

/*
 * CONFIG_CONSOLE_MUX drags in SYS_CONSOLE_IS_IN_ENV, meaning U-Boot takes the
 * console from the environment and IGNORES stdout-path in the device tree.
 * This device has nowhere to store the environment ("Loading Environment from
 * nowhere"), so without setting it here the whole early stage produces no
 * output at all - not even into RAM.
 *
 * `serial` is the serial_memlog driver (writes into the ramoops console zone,
 * readable from Linux after a reset); `vidconsole` is the screen through the
 * simple-framebuffer lk has already brought up.
 */
/*
 * booti patches these bootargs into the dtb's chosen node itself before handing
 * over, so the dtb needs no editing. The mainline dtb has an empty chosen, and
 * a missing root= panics the kernel right after init: "VFS: Unable to mount
 * root fs".
 *
 * Do NOT use /dev/mmcblkN: the two mmc controllers probe asynchronously so the
 * SD card is sometimes mmcblk0, sometimes mmcblk1 - either guess is wrong some
 * of the time. The PARTUUID comes from the card's MBR table signature (the
 * kernel prints "mmcblk1p2 00000000-02") and is stable across every boot. The
 * eMMC uses GPT so its GUIDs are entirely different, no risk of a mix-up.
 */
/*
 * `ro` IS DELIBERATE in the memory-extension test builds: if memory gets
 * overwritten, a read-only root cannot destroy the card — two cards were
 * already lost re-creating them after testing with `rw`. Switch back to `rw`
 * once a step has passed the load test (10 minutes under load, clean dmesg).
 * Do NOT put comments inside the macro: lines joined with `\` are cut off at
 * the very first line of the comment.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0" \
	"stdin=serial,button-kbd\0" \
	"bootmenu_0=Key states=button list; pause\0" \
	"bootmenu_1=Board info=bdinfo; pause\0" \
	"bootmenu_2=Reset=reset\0" \
	"bootargs=console=tty0 earlycon root=PARTUUID=1ace1007-02 rootwait rw " \
		"loglevel=4 clk_ignore_unused pd_ignore_unused audit=0\0"

#endif /* __MT6768_H */
