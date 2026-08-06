// SPDX-License-Identifier: GPL-2.0+
/*
 * Console that writes into a fixed physical address instead of a UART.
 *
 * The Redmi 9 exposes no usable UART without opening the case, so a U-Boot that
 * dies before video is up leaves no trace at all. This driver appends every
 * console byte to a ring in RAM that nothing else touches, so the next OS to
 * boot can read back what happened.
 *
 * The buffer is the console zone of the ramoops reservation:
 *
 *   mblock-9-pstore   0x4d010000  size 0xe0000
 *   console zone      0x4d05f000   -- verified in situ: holds the DBGC
 *                                     signature and the previous boot's text
 *
 * The tail of the framebuffer reservation looks free but is not: lk paints its
 * boot logo across it, so it would erase the log on the reset that follows a
 * hang, before Linux ever gets to read it.
 *
 * The format is Linux's persistent_ram_buffer, so the log comes back out of
 * /sys/fs/pstore/console-ramoops on the next boot with no tooling at all.
 */

#include <errno.h>
#include <linux/types.h>
#include <dm.h>
#include <serial.h>
#include <asm/io.h>

#define MEMLOG_BASE	0x4d05f000	/* ramoops console zone */
#define MEMLOG_SIZE	0x00040000	/* 256 KiB */
#define MEMLOG_MAGIC	0x43474244	/* "DBGC" -- Linux PERSISTENT_RAM_SIG */

/*
 * struct persistent_ram_buffer from Linux: sig, then start and size (atomic_t
 * there, plain u32 on the wire), then the text. Appending without wrapping
 * keeps a truncated log readable if U-Boot dies mid-write.
 */
struct memlog_hdr {
	u32 magic;
	u32 len;	/* persistent_ram calls this `start` */
	u32 mirror;	/* persistent_ram calls this `size`; must track `len` */
};

#define MEMLOG_TEXT	(MEMLOG_BASE + 12)
#define MEMLOG_TEXT_MAX	(MEMLOG_SIZE - 12)

static void memlog_reset(void)
{
	struct memlog_hdr __iomem *h = (void *)(uintptr_t)MEMLOG_BASE;

	writel(MEMLOG_MAGIC, &h->magic);
	writel(0, &h->len);
	writel(0, &h->mirror);
}

static void memlog_putc_raw(int ch)
{
	struct memlog_hdr __iomem *h = (void *)(uintptr_t)MEMLOG_BASE;
	u32 len;

	if (readl(&h->magic) != MEMLOG_MAGIC)
		memlog_reset();

	len = readl(&h->len);
	if (len >= MEMLOG_TEXT_MAX)
		return;			/* full: keep the beginning, drop the rest */

	writeb(ch, (void *)(uintptr_t)(MEMLOG_TEXT + len));
	writel(len + 1, &h->len);
	writel(len + 1, &h->mirror);
}

#ifdef CONFIG_DEBUG_UART_MEMLOG

#include <debug_uart.h>

static inline void _debug_uart_init(void)
{
	memlog_reset();
}

static inline void _debug_uart_putc(int ch)
{
	memlog_putc_raw(ch);
}

DEBUG_UART_FUNCS

#endif /* CONFIG_DEBUG_UART_MEMLOG */

static int memlog_serial_putc(struct udevice *dev, const char ch)
{
	memlog_putc_raw(ch);
	return 0;
}

static int memlog_serial_pending(struct udevice *dev, bool input)
{
	return 0;
}

static int memlog_serial_getc(struct udevice *dev)
{
	return -EAGAIN;
}

static int memlog_serial_setbrg(struct udevice *dev, int baudrate)
{
	return 0;
}

static int memlog_serial_probe(struct udevice *dev)
{
	/*
	 * Do not reset here. When the debug UART is enabled it has already
	 * logged the early boot, and probe happens much later.
	 */
	return 0;
}

static const struct dm_serial_ops memlog_serial_ops = {
	.putc = memlog_serial_putc,
	.pending = memlog_serial_pending,
	.getc = memlog_serial_getc,
	.setbrg = memlog_serial_setbrg,
};

static const struct udevice_id memlog_serial_ids[] = {
	{ .compatible = "u-boot,serial-memlog" },
	{ }
};

U_BOOT_DRIVER(serial_memlog) = {
	.name = "serial_memlog",
	.id = UCLASS_SERIAL,
	.of_match = memlog_serial_ids,
	.probe = memlog_serial_probe,
	.ops = &memlog_serial_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
