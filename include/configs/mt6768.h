#ifndef __MT6768_H
#define __MT6768_H

#include <linux/stringify.h>

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
	"con_in=serial,button-kbd,usbacm\0" \
	"con_out=serial,vidconsole,usbacm\0" \
	"stdin=serial,button-kbd,usbacm\0" \
	"stdout=serial,vidconsole,usbacm\0" \
	"stderr=serial,vidconsole,usbacm\0" \
	"bootmenu_default=4\0" \
	"bootmenu_0=Boot NixOS (microSD)=run bootnixos\0" \
	"bootmenu_1=Key states=button list; pause\0" \
	"bootmenu_2=Board info=bdinfo; pause\0" \
	"bootmenu_3=Show log=memlog 1800; pause\0" \
	"bootmenu_4=USB fastboot (stage + continue chainloads)=run fbchain\0" \
	"bootmenu_5=USB mass storage (microSD)=run umssd\0" \
	"bootmenu_6=U-Boot prompt=setenv menu_off 1\0" \
	"bootmenu_7=Reset=reset\0" \
	"scriptaddr=0x46000000\0" \
	"pxefile_addr_r=0x46100000\0" \
	"bootmeths=extlinux\0" \
	"boot_targets=mmc1\0" \
	"musb_quiesce=mw.l 112000a4 0; mw.w 11200006 0; mw.w 11200008 0; " \
		"mw.b 1120000b 0; mw.b 11200001 0\0" \
	"bootlamuc=load mmc 1:1 ${kernel_addr_r} /lamuc/Image && " \
		"load mmc 1:1 ${fdt_addr_r} /lamuc/mt6768-motorola-lamuc.dtb && " \
		"run usb_off && run musb_quiesce && " \
		"booti ${kernel_addr_r} - ${fdt_addr_r}\0" \
	"bootnixos=if load mmc 1:1 ${scriptaddr} /lamuc/menu.txt; then " \
		"for i in 0 1 2 3 4 5 6 7 8 9 10 11; do setenv bootmenu_${i}; done; " \
		"env import -t ${scriptaddr} ${filesize}; fi\0" \
	"bootdefault=run usb_off; run musb_quiesce; bootflow scan\0" \
	"usb_off=setenv stdin serial,button-kbd; setenv stdout serial,vidconsole; " \
		"setenv stderr serial,vidconsole\0" \
	"usb_on=setenv stdin ${con_in}; setenv stdout ${con_out}; " \
		"setenv stderr ${con_out}\0" \
	"umssd=run usb_off; ums 0 mmc 1; run usb_on\0" \
	"fbchain=run usb_off; setenv filesize; fastboot usb 0; " \
		"if test -n \"${after_fb}\"; then run after_fb; setenv after_fb; fi; " \
		"run usb_on; if test -n \"${filesize}\"; " \
		"then chainload " __stringify(CONFIG_FASTBOOT_BUF_ADDR) "; fi\0" \
	"bootargs=console=tty0 earlycon root=PARTUUID=1ace1007-02 rootwait rw " \
		"loglevel=4 clk_ignore_unused pd_ignore_unused audit=0\0"

#endif /* __MT6768_H */
