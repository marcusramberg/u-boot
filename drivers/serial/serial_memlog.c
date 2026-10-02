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
 * A reg property in the DT node overrides the lancelot address.
 */

#include <command.h>
#include <cyclic.h>
#include <errno.h>
#include <malloc.h>
#include <vsprintf.h>
#include <linux/types.h>
#include <dm.h>
#include <serial.h>
#include <asm/io.h>

#define MEMLOG_BASE	0x4d05f000	/* lancelot console zone, if DT has no reg */
#define MEMLOG_SIZE	0x00040000	/* 256 KiB */
#define MEMLOG_MAGIC	0x43474244	/* "DBGC" -- Linux PERSISTENT_RAM_SIG */

/*
 * struct persistent_ram_buffer from Linux: sig, the write offset and the
 * number of valid bytes, then a ring of text. Linux reads it back oldest
 * first as data[start..size) + data[0..start).
 */
struct memlog_hdr {
	u32 magic;
	u32 start;
	u32 size;
	u8 data[];
};

/* .data, not .bss: set before relocation and carried across it */
static ulong memlog_base __section(".data") = MEMLOG_BASE;
static ulong memlog_cap __section(".data") = MEMLOG_SIZE - sizeof(struct memlog_hdr);

static void memlog_reset(void)
{
	struct memlog_hdr __iomem *h = (void *)memlog_base;

	writel(MEMLOG_MAGIC, &h->magic);
	writel(0, &h->start);
	writel(0, &h->size);
}

static void memlog_putc_raw(int ch)
{
	struct memlog_hdr __iomem *h = (void *)memlog_base;
	u32 start, size;

	start = readl(&h->start);
	size = readl(&h->size);
	if (readl(&h->magic) != MEMLOG_MAGIC || start >= memlog_cap ||
	    size > memlog_cap) {
		memlog_reset();
		start = 0;
		size = 0;
	}

	writeb(ch, &h->data[start]);
	writel(start + 1 == memlog_cap ? 0 : start + 1, &h->start);
	if (size < memlog_cap)
		writel(size + 1, &h->size);
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
	fdt_size_t size;
	fdt_addr_t base = dev_read_addr_size(dev, &size);

	/*
	 * Do not reset here: the ring keeps earlier boots, which is the point
	 * after a hang, and the debug UART may already have logged this one.
	 */
	if (base != FDT_ADDR_T_NONE && size > sizeof(struct memlog_hdr)) {
		memlog_base = base;
		memlog_cap = size - sizeof(struct memlog_hdr);
	}

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

/* Print the newest part of the ring, e.g. what a hung earlier boot logged. */
static int do_memlog(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	struct memlog_hdr *h = (void *)memlog_base;
	ulong want = argc > 1 ? hextoul(argv[1], NULL) : 0x1000;
	ulong skip = argc > 2 ? hextoul(argv[2], NULL) : 0;
	u32 start = h->start, size = h->size, i;
	char *buf, *line;

	if (h->magic != MEMLOG_MAGIC || start >= memlog_cap || size > memlog_cap)
		return CMD_RET_FAILURE;
	if (skip > size)
		skip = size;
	if (want > size - skip)
		want = size - skip;

	/* snapshot first: printing appends to the same ring */
	buf = malloc(want + 1);
	if (!buf)
		return CMD_RET_FAILURE;
	for (i = 0; i < want; i++)
		buf[i] = h->data[(start + 2 * memlog_cap - skip - want + i) %
				 memlog_cap];
	buf[want] = 0;
	/* line by line: a slow console must not starve the watchdog */
	for (line = buf; line; ) {
		char *nl = strchr(line, '\n');

		if (nl)
			*nl = 0;
		puts(line);
		putc('\n');
		schedule();
		line = nl ? nl + 1 : NULL;
	}
	free(buf);

	return 0;
}

U_BOOT_CMD(memlog, 3, 0, do_memlog, "print the end of the RAM console",
	   "[bytes [skip]] (hex; default 1000 bytes, skip from the newest end)");
