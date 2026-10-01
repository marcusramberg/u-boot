#include <config.h>
#include <asm/global_data.h>
#include <asm/armv8/mmu.h>
#include <init.h>

#include <linux/bitops.h>
#include <linux/sizes.h>
#include <linux/libfdt.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <vsprintf.h>
#include <asm/io.h>
#include <dm.h>
#include <env.h>
#include <command.h>
#include <video.h>
#include <version_string.h>
#include <dm/device-internal.h>
#include <dm/uclass-internal.h>
#include <linux/delay.h>

/* defined in board/mediatek/mt6768/mt6768.c */
void mt6768_trace(const char *msg);

DECLARE_GLOBAL_DATA_PTR;

/*
 * TWO different RAM figures, deliberately so. Mixing them up hangs the device.
 *
 * 1) gd->ram_size — THE REGION U-BOOT IS ALLOWED TO USE.
 *    Taken from the /memory node of the built-in DTS (mt6768-generic.dts):
 *    0x3dcb0000, i.e. 0x40000000..0x7dcb0000. U-Boot relocates to the TOP of
 *    this region and the MMU only maps that far (mt6768_mem_map[1].size). It
 *    must NOT be raised, or U-Boot pushes itself outside the mapped region
 *    and hangs the moment the MMU is enabled.
 *
 * 2) MT6768_LANCELOT_DRAM_* — the REAL RAM, only written into the Linux dtb.
 *    Linux manages its own MMU, so it can use all of it.
 *
 * EVIDENCE for the real RAM figure: read straight out of the FDT LK left
 * behind (0x4bc80000), reported via /chosen/u-boot,lk-memory by the v4 build
 * and read back on the real device:
 *
 *   u-boot,lk-memory = 40000000+c0000000
 *
 * RAM = 0x40000000..0x100000000, exactly 3 GiB.
 *
 * (An earlier round inferred 0xbfff6000 from a downstream log and used that
 *  figure — WRONG. 0xbfff6000 is what the downstream kernel can still use
 *  AFTER SUBTRACTING the reserved regions, not the physical RAM range. No
 *  more inferring: read it straight from LK.)
 */
#define MT6768_LANCELOT_DRAM_BASE	0x40000000ULL

/*
 * ==========================================================================
 * SIZE DECLARED TO LINUX — CHANGE IT HERE, ONE LINE
 * ==========================================================================
 *
 * WARNING FROM A REAL CORRUPTION. Declaring 0xbfff6000 (3 GiB) outright DID
 * corrupt the SD card: EXT4 reported "bad extent address block", binaries read
 * back as garbage (SIGILL / "Exec format error") while the bytes on disk were
 * still fine; it took two e2fsck runs and re-creating the card.
 *
 * Reason: the TOTAL size figure is CORRECT, but it is NOT ENOUGH. Inside the
 * range 0x40000000..0xffff6000 there are holes taken by hardware/firmware that
 * the mainline DTS does not declare in reserved-memory. The mainline DTS has
 * only 6 regions, all below 0x80000000, while LK plants at least 12 (the
 * downstream log has mblock-5, mblock-8, mblock-9, mblock-10, mblock-11).
 * Declaring RAM on top of a firmware region in use gives exactly "random
 * corruption, a different one every time".
 *
 * LESSON: knowing the total size is NOT ENOUGH to declare memory. The list of
 * firmware holes sitting inside it has to be known too.
 *
 * MEASURED ON THE DEVICE (v4 build, reading the FDT LK left at 0x4bc80000):
 *
 *   u-boot,lk-memory = 40000000+c0000000
 *
 * Real RAM = 0x40000000..0x100000000, EXACTLY 3 GiB. Not 0xbfff6000 — that
 * figure is what the downstream kernel may still use AFTER SUBTRACTING the
 * reserved regions, not the physical RAM range.
 *
 * Cross-check, and it fits exactly: the top of what the downstream kernel uses
 * is 0xffff6000; next comes the ION carveout 0xffff6000+0x9000 = 0xfffff000;
 * next mblock-2-dramc-rk1 0xfffff000+0x1000 = 0x100000000. Contiguous, no gap.
 *
 * LK also declares 18 reserved regions, 12 of which the mainline DTS lacks,
 * and 7 of those sit ABOVE 0x80000000 — including mblock-16-ccci
 * 0xae000000+0x10000000 (256 MiB of memory shared with the modem). That is
 * what corrupted the SD card.
 *
 * U-Boot now copies that list straight from LK into the Linux dtb
 * (mt6768_copy_lk_reserved), so no figure has to be transcribed by hand.
 *
 * CURRENT STEP — change one line below:
 *   SAFE  0x3e605000  ~998 MiB  — as-is, known to run stably
 *   1G    0x40000000  up to 0x80000000
 *   2G    0x80000000  up to 0xc0000000
 *   FULL  0xc0000000  up to 0x100000000  — exactly 3 GiB, LK's real figure
 *
 * This is a CAP, not an imposed value: the real size is taken from LK's FDT
 * and then clamped down to this cap. If LK cannot be read, fall back to SAFE.
 */
#define MT6768_RAM_SIZE_SAFE		0x3e605000ULL
#define MT6768_RAM_SIZE_1G		0x40000000ULL
#define MT6768_RAM_SIZE_2G		0x80000000ULL
#define MT6768_RAM_SIZE_FULL		0xc0000000ULL

#define MT6768_RAM_SIZE_CAP		MT6768_RAM_SIZE_FULL
#define MT6768_LANCELOT_DRAM_SIZE	MT6768_RAM_SIZE_SAFE

/*
 * DRAM window mapped by U-Boot's MMU (= gd->ram_size from the built-in DTS).
 * Only this range may be read/written, anything else is an access fault.
 */
#define MT6768_UBOOT_MAP_SIZE		0x3dcb0000ULL

/*
 * Where the Linux dtb already sits in RAM by the time U-Boot runs.
 *
 * LK loads the ramdisk part of the boot image to 0x47c80000 (the raddr field
 * in the header, read back from the boot image itself). pack-uboot-payload.py
 * places the kernel at the start of that area and the dtb at
 * DTB_OFFSET = 0x2400000. So the dtb already sits at:
 *     0x47c80000 + 0x2400000 = 0x4a080000
 * The same address bootcmd uses in `cp.b 0x4a080000 0x60000000 0x10000`.
 *
 * This region lies inside 0x40000000..0x7dcb0000, so U-Boot's MMU maps it and
 * it can be read and written normally.
 */
#define MT6768_PAYLOAD_DTB_ADDR		0x4a080000UL
#define MT6768_PAYLOAD_SCAN_BEG		0x47c80000UL
#define MT6768_PAYLOAD_SCAN_END		0x4d000000UL

static struct mm_region mt6768_mem_map[] = {
	{
		/* Peripherals */
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = 0x40000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) | PTE_BLOCK_NON_SHARE |
		         PTE_BLOCK_PXN | PTE_BLOCK_UXN },
	{
		/* DDR */
		.virt = 0x40000000UL,
		.phys = 0x40000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_OUTER_SHARE,
	},
	{
		/* Framebuffer */
		/* virt/phys get updated in dram_init()*/
		.size = 0x00c00000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) | PTE_BLOCK_INNER_SHARE |
		         PTE_BLOCK_PXN | PTE_BLOCK_UXN },
	{
		/* sentinel*/
		0,
	}
};
struct mm_region *mem_map = mt6768_mem_map;

static const void *get_prevbl_fdt_addr(void)
{
	const char *fdt_addr_str = env_get("prevbl_fdt_addr");

	if (!fdt_addr_str)
		return NULL;

	return (const void *)simple_strtoul(fdt_addr_str, NULL, 16);
}

static const char *get_cmdline(void)
{
	const void *fdt_blob = get_prevbl_fdt_addr();
	int node;

	if (!fdt_blob)
		return NULL;

	if (fdt_check_header(fdt_blob))
		return NULL;

	node = fdt_path_offset(fdt_blob, "/chosen");
	return fdt_getprop(fdt_blob, node, "bootargs", NULL);
}

static int get_cmdline_option(const char *cmdline, const char *key, char *out,
			      int out_len)
{
	const char *p, *p_end;
	int len;

	p = strstr(cmdline, key);
	if (!p)
		return -ENOENT;

	p += strlen(key);
	p_end = strstr(p, " ");
	if (!p_end)
		return -ENOENT;

	len = p_end - p;
	if (len > out_len)
		len = out_len;

	strncpy(out, p, len);
	out[len] = '\0';

	return 0;
}

/*
 * Make room in the blob, but NEVER past `limit`.
 *
 * `limit` is the size of the buffer that ACTUALLY exists, not an estimate:
 *
 *  - Early path (board_late_init): the dtb sits at 0x4a080000 and bootcmd
 *    copies it with `cp.b ... 0x10000`, so the buffer is 64 KiB
 *    -> limit = MT6768_PAYLOAD_WINDOW.
 *
 *  - booti path: boot_relocate_fdt() has already allocated exactly
 *    of_len = ft_len + CONFIG_SYS_FDT_PAD (0x3000) and set totalsize = of_len.
 *    That is, libfdt ALREADY knows it has 12 KiB of headroom. Asking for more
 *    is a claim with nothing behind it: fdt_open_into() only rewrites the
 *    totalsize field, it does not allocate anything, so subsequent writes run
 *    past the end of the region LMB handed out.
 *    -> pass limit = 0, i.e. do not grow at all.
 *
 * This was a real bug in v6: after relocation totalsize was 61173 and three
 * fdt_open_into() calls added another 10752 bytes on top, overrunning the
 * buffer. It only worked by luck, because the memory next to it happened to be
 * free; printing the real addresses is what exposed it.
 */
#define MT6768_PAYLOAD_WINDOW	0x10000u

static int mt6768_grow(void *blob, unsigned int extra, unsigned int limit)
{
	unsigned int want = fdt_totalsize(blob) + extra;

	if (!extra || want > limit)
		return 0;

	return fdt_open_into(blob, blob, want);
}

/*
 * Return the FDT LK left behind, or NULL. Bounds-check before touching it:
 * only the DRAM window mapped by U-Boot's MMU may be read.
 */
static const void *mt6768_lk_fdt(void)
{
	unsigned long lk = (unsigned long)get_prev_bl_fdt_addr();

	if (lk < MT6768_LANCELOT_DRAM_BASE ||
	    lk >= MT6768_LANCELOT_DRAM_BASE + MT6768_UBOOT_MAP_SIZE)
		return NULL;
	if (fdt_check_header((const void *)lk))
		return NULL;

	return (const void *)lk;
}

/* Read an address/size value made of `cells` cells (big endian). */
static u64 mt6768_read_cells(const fdt32_t *p, int cells)
{
	u64 v = 0;

	while (cells-- > 0)
		v = (v << 32) | fdt32_to_cpu(*p++);

	return v;
}

/*
 * Find the memory node. Do NOT rely on fdt_path_offset("/memory") as the main
 * path: the real node is named "memory@40000000", and the safest way is to
 * look for the very thing the Linux kernel uses to recognise memory — the
 * device_type = "memory" property.
 *
 * `count` returns how many such nodes exist at the root, to spot duplicates.
 */
static int mt6768_find_memory_node(const void *blob, int *count)
{
	int node, found = -1, n = 0;
	const char *dt;

	fdt_for_each_subnode(node, blob, 0) {
		dt = fdt_getprop(blob, node, "device_type", NULL);
		if (dt && !strcmp(dt, "memory")) {
			n++;
			if (found < 0)
				found = node;
		}
	}

	*count = n;

	if (found < 0)
		found = fdt_path_offset(blob, "/memory");

	return found;
}

/*
 * Write the real RAM size into a dtb, plus a "confirmation marker" under
 * /chosen so that from Linux it is visible which code path ran AND what came
 * out — this device has no reliable way to read the U-Boot log, so it has to
 * report through the dtb itself.
 *
 * Once Linux is up, check with:
 *     cat /proc/device-tree/chosen/u-boot,ram-fixup-early
 *     cat /proc/device-tree/chosen/u-boot,ram-fixup-booti
 *
 * `marker` is the property name, different per call path, so if both run then
 * both are visible and neither hides the other.
 *
 * WRITE THE reg PROPERTY DIRECTLY, do NOT use fdt_fixup_memory().
 *
 * An earlier round used fdt_fixup_memory() and it failed silently. Reason:
 * include/fdt_support.h
 *
 *     #ifdef CONFIG_ARCH_FIXUP_FDT_MEMORY
 *     int fdt_fixup_memory_banks(void *blob, u64 start[], u64 size[], int banks);
 *     #else
 *     static inline int fdt_fixup_memory_banks(void *blob, u64 start[],
 *                                              u64 size[], int banks)
 *     {
 *             return 0;
 *     }
 *     #endif
 *
 * The real fdt_fixup_memory_banks() in boot/fdt_support.c also sits inside the
 * same #ifdef (lines 496..607), while fdt_fixup_memory() (line 609) sits
 * OUTSIDE it. This configuration does not enable CONFIG_ARCH_FIXUP_FDT_MEMORY,
 * so fdt_fixup_memory() compiles down to "return 0" — it does nothing yet
 * reports success. That is why the earlier round managed to plant the marker
 * under /chosen while reg stayed unchanged.
 *
 * The fix: write it here directly. CONFIG_ARCH_FIXUP_FDT_MEMORY stays off,
 * because enabling it would drag in arch_fixup_fdt() writing gd->bd->bi_dram[]
 * (0x3dcb0000 — the figure meant for relocation) into this very node, leaving
 * the result dependent on call order. Writing it directly depends on nothing.
 */
/*
 * Read the real RAM range from the first memory node of the FDT LK handed on.
 * Returns 0 if it could be read.
 */
static int mt6768_lk_ram(u64 *base, u64 *size)
{
	const void *f = mt6768_lk_fdt();
	const fdt32_t *p;
	int node, ac, sc, len, cnt = 0;

	if (!f)
		return -1;

	ac = fdt_address_cells(f, 0);
	sc = fdt_size_cells(f, 0);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
		return -1;

	fdt_for_each_subnode(node, f, 0) {
		const char *dt = fdt_getprop(f, node, "device_type", NULL);

		if (!dt || strcmp(dt, "memory"))
			continue;
		cnt++;
		p = fdt_getprop(f, node, "reg", &len);
		if (!p || len < (ac + sc) * (int)sizeof(*p))
			continue;
		*base = mt6768_read_cells(p, ac);
		*size = mt6768_read_cells(p + ac, sc);
		/*
		 * Take only the FIRST range. On this device LK reports
		 * exactly one (nmem=1, 40000000+c0000000). If there are ever
		 * several, they need separate handling — the confirmation
		 * marker reports the count so that is visible.
		 */
		return cnt;
	}

	return -1;
}

/*
 * Copy LK's reserved regions into the Linux dtb.
 *
 * LK plants 18 mblock-N-* nodes with FIXED addresses into its FDT — that is
 * the real map of what hardware/firmware occupies. The mainline DTS only
 * transcribes 6 regions by hand; the remaining 12 (291.9 MiB, of which
 * mblock-16-ccci is 256 MiB of memory shared with the modem) go undeclared,
 * and that is what corrupted the SD card when RAM was extended.
 *
 * Copy from LK instead of by hand: the figures cannot be wrong, and they
 * follow along automatically if LK changes.
 *
 * SKIP RULE: if an LK region overlaps ANY node already present in the target
 * dtb's /reserved-memory, skip it. Reason: mainline splits the region
 * 0x4d000000..0x4d100000 into three nodes differently from LK
 * (ramoops@4d05f000 must keep its exact address because that is the log
 * channel), but the UNION of both splits is identical. Adding overlapping
 * entries gains nothing and could break the log channel. Verified: exactly 6
 * LK regions are skipped for this reason (atf, ram_console, pstore, minirdump,
 * tee, framebuffer), the remaining 12 touch nothing at all.
 */
/*
 * MUST BE CALLED FROM board_late_init(), NOT only from ft_board_setup().
 *
 * boot/image-board.c image_setup_linux() runs in exactly this order:
 *
 *   1. boot_fdt_add_mem_rsv_regions(*of_flat_tree);   <- load into LMB
 *   2. boot_relocate_fdt(of_flat_tree, &of_size);     <- pick a spot for dtb
 *   3. image_setup_libfdt(...) -> ft_board_setup()    <- our code
 *
 * Step 1 reads BOTH the memreserve block AND the dtb's /reserved-memory nodes
 * and holds them in LMB; step 2 then steers clear of them. Adding entries only
 * at step 3 is too late — LMB has already placed the dtb.
 *
 * This was paid for for real: gd->ram_size = 0x3dcb0000 puts the top of LMB at
 * 0x7dcb0000, and boot_relocate_fdt() placed the dtb (49152 bytes) at
 * 0x7c80c000 — right in the middle of mblock-18-ccci (0x7c000000..0x7cc00000,
 * memory shared with the modem). The kernel reserves the dtb region first, so
 * when the overlapping no-map node mblock-18-ccci came up it was rejected
 * (-EBUSY) and the protection was lost entirely.
 *
 * Hence: copy from board_late_init(), into the original dtb still sitting at
 * 0x4a080000, BEFORE bootcmd `cp.b`s it to 0x60000000. That cures a whole
 * class of problems — LMB then avoids LK's regions for EVERYTHING it
 * allocates (dtb, initrd, cmdline), not just the dtb.
 */
static int mt6768_copy_lk_reserved(void *blob, int *added, int *skipped,
				   int *rsv, unsigned int limit)
{
	const void *f = mt6768_lk_fdt();
	const fdt32_t *p;
	int lk_resv, dst_resv, sub, dst, ac, sc, len, err;
	u64 base, size;

	*added = 0;
	*skipped = 0;
	*rsv = 0;

	if (!f)
		return -1;

	/* Make room ourselves: 12 nodes + 12 memreserve entries. */
	err = mt6768_grow(blob, 4096, limit);
	if (err)
		return err;

	ac = fdt_address_cells(f, 0);
	sc = fdt_size_cells(f, 0);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
		return -1;

	lk_resv = fdt_path_offset(f, "/reserved-memory");
	if (lk_resv < 0)
		return -1;

	fdt_for_each_subnode(sub, f, lk_resv) {
		const char *name = fdt_get_name(f, sub, NULL);
		int overlap = 0;
		fdt32_t cells[4];
		int n = 0;

		p = fdt_getprop(f, sub, "reg", &len);
		if (!p || len < (ac + sc) * (int)sizeof(*p))
			continue;
		base = mt6768_read_cells(p, ac);
		size = mt6768_read_cells(p + ac, sc);
		if (!size)
			continue;

		/* Skip if it overlaps any region already present. */
		dst_resv = fdt_path_offset(blob, "/reserved-memory");
		if (dst_resv >= 0) {
			fdt_for_each_subnode(dst, blob, dst_resv) {
				const fdt32_t *q;
				u64 b2, s2;
				int l2;

				q = fdt_getprop(blob, dst, "reg", &l2);
				if (!q || l2 < (ac + sc) * (int)sizeof(*q))
					continue;
				b2 = mt6768_read_cells(q, ac);
				s2 = mt6768_read_cells(q + ac, sc);
				if (base < b2 + s2 && b2 < base + size) {
					overlap = 1;
					break;
				}
			}
		}
		if (overlap) {
			(*skipped)++;
			continue;
		}

		/*
		 * Create the new node. LK's name is kept as-is so it can be
		 * cross-checked later, only prefixed to show where it came
		 * from.
		 */
		dst_resv = fdt_path_offset(blob, "/reserved-memory");
		if (dst_resv < 0) {
			/* Create it if absent, with the root cell counts. */
			fdt32_t c;

			dst_resv = fdt_add_subnode(blob, 0, "reserved-memory");
			if (dst_resv < 0)
				return dst_resv;
			c = cpu_to_fdt32(ac);
			fdt_setprop(blob, dst_resv, "#address-cells", &c,
				    sizeof(c));
			c = cpu_to_fdt32(sc);
			fdt_setprop(blob, dst_resv, "#size-cells", &c,
				    sizeof(c));
			fdt_setprop_empty(blob, dst_resv, "ranges");
		}

		dst = fdt_add_subnode(blob, dst_resv, name);
		if (dst == -FDT_ERR_EXISTS) {
			(*skipped)++;
			continue;
		}
		if (dst < 0)
			return dst;

		if (ac == 2)
			cells[n++] = cpu_to_fdt32((u32)(base >> 32));
		cells[n++] = cpu_to_fdt32((u32)base);
		if (sc == 2)
			cells[n++] = cpu_to_fdt32((u32)(size >> 32));
		cells[n++] = cpu_to_fdt32((u32)size);

		err = fdt_setprop(blob, dst, "reg", cells,
				  n * (int)sizeof(cells[0]));
		if (err)
			return err;

		/*
		 * Do NOT set no-map. And ALSO write into the FDT memreserve
		 * block.
		 *
		 * Why no-map is dropped — this is where mblock-18-ccci fell
		 * through; drivers/of/of_reserved_mem.c in kernel 6.18:
		 *
		 *   static int __init early_init_dt_reserve_memory(...)
		 *   {
		 *           if (nomap) {
		 *                   if (memblock_overlaps_region(&memblock.memory, base, size) &&
		 *                       memblock_is_region_reserved(base, size))
		 *                           return -EBUSY;
		 *                   return memblock_mark_nomap(base, size);
		 *           }
		 *           return memblock_reserve(base, size);
		 *   }
		 *
		 * Any `no-map` region that INTERSECTS a region reserved
		 * earlier is REJECTED outright (-EBUSY) — i.e. the protection
		 * is lost completely, the worst possible outcome. The
		 * non-no-map path calls memblock_reserve() straight away,
		 * **which cannot fail that way**.
		 *
		 * What is needed here is "the kernel must not allocate in
		 * here", and memblock_reserve() does exactly that. no-map only
		 * adds one further meaning, "keep out of the linear map" —
		 * valuable, but not worth risking silently losing the
		 * protection outright. The genuinely security-relevant regions
		 * (tee@70000000, atf@4ce00000) keep no-map because they are
		 * already in the mainline DTS and are skipped above.
		 *
		 * Writing into memreserve as well is a second layer, wholly
		 * independent of parsing the /reserved-memory nodes:
		 * early_init_fdt_scan_reserved_mem() handles the memreserve
		 * block FIRST, with a plain memblock_reserve(). It works even
		 * if the nodes are for some reason not parsed.
		 */
		err = fdt_add_mem_rsv(blob, base, size);
		if (!err)
			(*rsv)++;

		(*added)++;
	}

	return 0;
}

static int mt6768_fixup_ram(void *blob, const char *marker,
			    unsigned int limit)
{
	int node, ac, sc, len, n = 0, dtcnt = 0, ret_oi, ret_sp = 0;
	u64 rb_base = 0, rb_size = 0;
	u64 base = MT6768_LANCELOT_DRAM_BASE;
	u64 size = MT6768_LANCELOT_DRAM_SIZE;
	u64 lk_base = 0, lk_size = 0;
	const char *src = "hard";
	int nmem;
	const fdt32_t *p;
	fdt32_t cells[4];
	char info[224];

	/*
	 * The size comes from LK's FDT — the hardware source of truth — then is
	 * clamped to the cap of the current step. If LK cannot be read, fall
	 * back to the compiled-in safe figure. Never guess.
	 */
	nmem = mt6768_lk_ram(&lk_base, &lk_size);
	if (nmem > 0 && lk_base == MT6768_LANCELOT_DRAM_BASE && lk_size) {
		base = lk_base;
		size = lk_size > MT6768_RAM_SIZE_CAP ? MT6768_RAM_SIZE_CAP
						     : lk_size;
		src = (lk_size > MT6768_RAM_SIZE_CAP) ? "lk-clamp" : "lk";
	}

	/*
	 * Make room first. The new reg property is usually the same length as
	 * the old one so it needs none, but the /chosen marker does. The error
	 * code is recorded in the marker.
	 */
	ret_oi = mt6768_grow(blob, 1024, limit);

	node = mt6768_find_memory_node(blob, &dtcnt);
	if (node < 0) {
		printf("%s: no memory node found (err %d)\n", marker, node);
		return 0;
	}

	ac = fdt_address_cells(blob, 0);
	sc = fdt_size_cells(blob, 0);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2) {
		printf("%s: #address-cells=%d #size-cells=%d invalid\n",
		       marker, ac, sc);
		return 0;
	}

	if (ac == 2)
		cells[n++] = cpu_to_fdt32((u32)(base >> 32));
	cells[n++] = cpu_to_fdt32((u32)base);
	if (sc == 2)
		cells[n++] = cpu_to_fdt32((u32)(size >> 32));
	cells[n++] = cpu_to_fdt32((u32)size);

	ret_sp = fdt_setprop(blob, node, "reg", cells, n * sizeof(cells[0]));

	/* Read back from the blob: that is the evidence, not the retval. */
	p = fdt_getprop(blob, node, "reg", &len);
	if (p && len >= (ac + sc) * (int)sizeof(*p)) {
		rb_base = mt6768_read_cells(p, ac);
		rb_size = mt6768_read_cells(p + ac, sc);
	}

	snprintf(info, sizeof(info),
		 "%s c=%d/%d oi=%d sp=%d rb=%llx+%llx dt=%d src=%s lk=%llx+%llx nmem=%d cap=%llx",
		 fdt_get_name(blob, node, NULL), ac, sc, ret_oi, ret_sp,
		 (unsigned long long)rb_base, (unsigned long long)rb_size,
		 dtcnt, src, (unsigned long long)lk_base,
		 (unsigned long long)lk_size, nmem,
		 (unsigned long long)MT6768_RAM_SIZE_CAP);
	printf("%s: %s\n", marker, info);

	node = fdt_path_offset(blob, "/chosen");
	if (node < 0)
		node = fdt_add_subnode(blob, 0, "chosen");
	if (node >= 0)
		fdt_setprop_string(blob, node, marker, info);

	return 0;
}

/*
 * MEASUREMENT: report verbatim what LK left behind in its FDT.
 *
 * LK loads the boot image dtb to the `tags` address (0x4bc80000 on this
 * device, read from the boot image header), **and before jumping into the
 * "kernel" it patches in the real /memory node and the real /reserved-memory
 * list (the mblock-N-* nodes)**. The "kernel" as far as LK is concerned is
 * U-Boot, so register x0 at U-Boot entry points at that FDT. It is the
 * hardware source of truth, better than any guess made from a log.
 *
 * x0 is preserved by save_boot_params() in arch/arm/lib/save_prev_bl_data.c —
 * but only if CONFIG_SAVE_PREV_BL_FDT_ADDR is enabled. That config used to be
 * off, so save_prev_bl_data.o was not linked in (System.map only had the weak
 * `W save_boot_params`), and the whole LK-FDT path was dead code.
 *
 * This function writes NOTHING from LK into the Linux dtb — it only copies out
 * text to be read. Extending memory comes later, once the real list has been
 * seen.
 */
static char mt6768_lk_mem[224];
static char mt6768_lk_resv[1280];

static void mt6768_report_lk_fdt(void *blob, unsigned int limit)
{
	unsigned long lk = (unsigned long)get_prev_bl_fdt_addr();
	const void *f = (const void *)lk;
	const fdt32_t *p;
	char hdr[128];
	int node, sub, ac = 0, sc = 0, len, i, cnt = 0, off = 0;
	int valid = 0, nmem = 0, ret_oi = 0, r1 = 0, r2 = 0, r3 = 0;

	mt6768_lk_mem[0] = '\0';
	mt6768_lk_resv[0] = '\0';

	/* Only the region mapped by U-Boot's MMU may be read. */
	if (lk < MT6768_LANCELOT_DRAM_BASE ||
	    lk >= MT6768_LANCELOT_DRAM_BASE + MT6768_UBOOT_MAP_SIZE) {
		snprintf(hdr, sizeof(hdr), "x0=%#lx outside mapped region", lk);
		goto out;
	}
	if (fdt_check_header(f)) {
		snprintf(hdr, sizeof(hdr), "x0=%#lx not an FDT", lk);
		goto out;
	}

	valid = 1;
	ac = fdt_address_cells(f, 0);
	sc = fdt_size_cells(f, 0);
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2) {
		snprintf(hdr, sizeof(hdr), "x0=%#lx c=%d/%d odd, skipped",
			 lk, ac, sc);
		valid = 0;
		goto out;
	}

	/* LK's /memory: list ALL ranges, not just the first one. */
	fdt_for_each_subnode(node, f, 0) {
		const char *dt = fdt_getprop(f, node, "device_type", NULL);

		if (!dt || strcmp(dt, "memory"))
			continue;
		nmem++;
		p = fdt_getprop(f, node, "reg", &len);
		len /= (int)sizeof(*p);
		for (i = 0; p && i + ac + sc <= len; i += ac + sc) {
			if (off >= (int)sizeof(mt6768_lk_mem) - 40)
				break;
			off += snprintf(mt6768_lk_mem + off,
					sizeof(mt6768_lk_mem) - off, "%llx+%llx ",
					(unsigned long long)mt6768_read_cells(p + i, ac),
					(unsigned long long)mt6768_read_cells(p + i + ac, sc));
		}
	}

	/* LK's /reserved-memory: name + range, in full. The missing piece. */
	off = 0;
	node = fdt_path_offset(f, "/reserved-memory");
	fdt_for_each_subnode(sub, f, node) {
		p = fdt_getprop(f, sub, "reg", &len);
		if (!p || len < (ac + sc) * (int)sizeof(*p))
			continue;
		cnt++;
		if (off >= (int)sizeof(mt6768_lk_resv) - 72)
			continue;
		off += snprintf(mt6768_lk_resv + off,
				sizeof(mt6768_lk_resv) - off, "%s=%llx+%llx ",
				fdt_get_name(f, sub, NULL),
				(unsigned long long)mt6768_read_cells(p, ac),
				(unsigned long long)mt6768_read_cells(p + ac, sc));
	}

	snprintf(hdr, sizeof(hdr), "x0=%#lx ok c=%d/%d nmem=%d nresv=%d",
		 lk, ac, sc, nmem, cnt);
out:
	printf("LK FDT : %s\n", hdr);
	printf("  mem  : %s\n", mt6768_lk_mem);
	printf("  resv : %s\n", mt6768_lk_resv);

	/*
	 * Make room here, do not count on anyone else having done it. These
	 * three strings can reach ~1.6 KiB, and fdt_setprop() short on space
	 * fails SILENTLY — the very trap that cost an earlier test round.
	 */
	ret_oi = mt6768_grow(blob, 2560, limit);

	node = fdt_path_offset(blob, "/chosen");
	if (node < 0)
		node = fdt_add_subnode(blob, 0, "chosen");
	if (node < 0) {
		printf("%s: cannot create /chosen (%d)\n", __func__, node);
		return;
	}

	if (valid) {
		r1 = fdt_setprop_string(blob, node, "u-boot,lk-memory",
					mt6768_lk_mem);
		r2 = fdt_setprop_string(blob, node, "u-boot,lk-resv",
					mt6768_lk_resv);
	}

	/* Error codes of the writes above travel in the summary string. */
	off = strlen(hdr);
	snprintf(hdr + off, sizeof(hdr) - off, " oi=%d w=%d/%d",
		 ret_oi, r1, r2);
	r3 = fdt_setprop_string(blob, node, "u-boot,lk-fdt", hdr);

	printf("  write: oi=%d lk-memory=%d lk-resv=%d lk-fdt=%d\n",
	       ret_oi, r1, r2, r3);
}

/*
 * First fixup path: runs early and depends on nothing.
 *
 * The "by the book" path is ft_board_setup() during booti. But if for any
 * reason image_setup_libfdt() does not run, the whole fixup is dropped
 * silently. So patch the original dtb already sitting in RAM as well, BEFORE
 * bootcmd copies it to 0x60000000. U-Boot only has to reach board_late_init().
 *
 * Only touches a blob with a valid FDT magic; if none is found, do nothing.
 */
static void mt6768_fixup_payload_dtb(void)
{
	void *blob = (void *)MT6768_PAYLOAD_DTB_ADDR;
	ulong addr;

	if (fdt_check_header(blob)) {
		/*
		 * Not at the usual spot (DTB_OFFSET in
		 * pack-uboot-payload.py may have changed). Scan the ramdisk
		 * region LK loaded to find it. 4 KiB step, read-only, never
		 * touching anything outside the mapped region.
		 */
		blob = NULL;
		for (addr = MT6768_PAYLOAD_SCAN_BEG; addr < MT6768_PAYLOAD_SCAN_END;
		     addr += 0x1000) {
			if (fdt_check_header((void *)addr))
				continue;
			blob = (void *)addr;
			break;
		}
		if (!blob) {
			printf("%s: no dtb in the payload region, skipping\n",
			       __func__);
			return;
		}
		printf("%s: found dtb at %p (not the usual spot)\n",
		       __func__, blob);
	}

	if (fdt_path_offset(blob, "/memory") < 0) {
		printf("%s: blob at %p has no /memory node, skipping\n",
		       __func__, blob);
		return;
	}

	mt6768_fixup_ram(blob, "u-boot,ram-fixup-early", MT6768_PAYLOAD_WINDOW);

	/*
	 * Copy LK's reserved regions HERE, not waiting for ft_board_setup().
	 * See the long comment on mt6768_copy_lk_reserved(): they must be in
	 * the dtb before image_setup_linux() calls
	 * boot_fdt_add_mem_rsv_regions(), otherwise LMB places the dtb in the
	 * middle of a firmware region.
	 */
	{
		int added = 0, skipped = 0, rsv = 0, rc, cn;
		char msg[96];

		rc = mt6768_copy_lk_reserved(blob, &added, &skipped, &rsv,
					     MT6768_PAYLOAD_WINDOW);
		snprintf(msg, sizeof(msg), "rc=%d add=%d skip=%d memrsv=%d/%d",
			 rc, added, skipped, rsv, fdt_num_mem_rsv(blob));
		printf("%s: copy reserved-memory from LK: %s\n", __func__, msg);

		cn = fdt_path_offset(blob, "/chosen");
		if (cn >= 0)
			fdt_setprop_string(blob, cn, "u-boot,lk-resv-copy-early",
					   msg);
	}
}

/*
 * lk allocates the framebuffer at runtime; point /framebuffer in U-Boot's own
 * dtb at lk's mblock. Runs before relocation so the patched blob gets copied.
 */
static void mt6768_lk_fb_to_simplefb(void)
{
	const void *lk = (const void *)get_prev_bl_fdt_addr();
	int resv, sub, fb;
	fdt_addr_t base;
	fdt_size_t size;
	fdt64_t reg[2];
	u64 page;

	if (!lk || fdt_check_header(lk))
		return;
	resv = fdt_path_offset(lk, "/reserved-memory");
	fb = fdt_path_offset(gd->fdt_blob, "/framebuffer");
	if (resv < 0 || fb < 0)
		return;
	fdt_for_each_subnode(sub, lk, resv) {
		if (!strstr(fdt_get_name(lk, sub, NULL), "framebuffer"))
			continue;
		base = fdtdec_get_addr_size_auto_parent(lk, resv, sub, "reg",
							0, &size, false);
		if (base == FDT_ADDR_T_NONE)
			return;
		/*
		 * lk scans out its second page after fastboot `continue`.
		 * ponytail: fixed page guess, read the OVL scan-out address
		 * instead once the display block is mapped.
		 */
		page = ALIGN(fdtdec_get_uint(gd->fdt_blob, fb, "width", 0), 32) *
		       4 * fdtdec_get_uint(gd->fdt_blob, fb, "height", 0);
		reg[0] = cpu_to_fdt64(base + page);
		reg[1] = cpu_to_fdt64(fdtdec_get_uint(gd->fdt_blob, fb, "stride", 0) *
				      fdtdec_get_uint(gd->fdt_blob, fb, "height", 0));
		fdt_setprop_inplace((void *)gd->fdt_blob, fb, "reg", reg,
				    sizeof(reg));
		return;
	}
}

int dram_init(void)
{
	int ret = fdtdec_setup_mem_size_base();
	if (ret) {
		printf("%s: failed (err: %d)\n", __func__, ret);
		return ret;
	}

	/*
	 * Do not use get_ram_size(): it writes into memory to probe the limit,
	 * scans up to 8 GB, hits this device's secure regions and returns an
	 * untrue 4 GiB, which makes U-Boot relocate outside physical RAM and
	 * then hang.
	 *
	 * Nor take mt6768_mem_map[1].size: that DDR entry does not declare
	 * .size so it is 0. Keep the value fdtdec_setup_mem_size_base() read
	 * from the device tree; the end of this function feeds it back into
	 * mem_map for the MMU.
	 */
	if (IS_ENABLED(CONFIG_MOTOROLA_LAMUC))
		mt6768_lk_fb_to_simplefb();

	/* build the memmap */
	int simplefb = fdt_path_offset(gd->fdt_blob, "/framebuffer");
	if (simplefb >= 0) {
		fdt_size_t fb_size = 0;
		fdt_addr_t base = fdtdec_get_addr_size_auto_noparent(
			gd->fdt_blob, simplefb, "reg", 0, &fb_size, false);

		/*
		 * Must use a helper that honours #address-cells.
		 * fdtdec_get_addr() assumes a one-cell address while this DTS
		 * uses two, so it returns 0 and mem_map[2] would land on the
		 * device region of mem_map[0] -> hang when the MMU is enabled.
		 */
		if (base == FDT_ADDR_T_NONE || base == 0) {
			printf("%s: bad framebuffer reg, skipping mapping\n",
			       __func__);
		} else {
			mt6768_mem_map[2].virt = (u64)base;
			mt6768_mem_map[2].phys = (u64)base;
			if (fb_size)
				mt6768_mem_map[2].size = (u64)fb_size;
		}
	} else {
		printf("%s: no simplefb node in fdt\n", __func__);
	}

	mt6768_trace("[UBOOT] dram_init: done\n");
	mt6768_mem_map[1].size = gd->ram_size;

	/*
	 * There used to be a call here
	 *   fdt_fixup_memory(gd->fdt_blob, CFG_SYS_SDRAM_BASE, gd->ram_size);
	 * It is gone. It wrote into gd->fdt_blob — U-BOOT'S OWN built-in dtb
	 * (CONFIG_OF_SEPARATE), not the dtb handed to Linux — so it only wrote
	 * back the very figure just read out of it. Pointless, and misleading
	 * in suggesting U-Boot decides Linux's RAM size.
	 *
	 * The place to patch the Linux dtb is ft_board_setup() at the end of
	 * this file, which runs during booti (needs CONFIG_OF_BOARD_SETUP=y).
	 */
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = gd->ram_base;
	gd->bd->bi_dram[0].size = gd->ram_size;
	return 0;
}

void reset_cpu(void)
{
	printf("resetting ...\n");
	/* wdt reset: */
	/* reset counter */
	writel(0x1971, 0x10007000 + 0x8);
	/* SW reset */
	writel(0x1209, 0x10007000 + 0x14);
}

int board_init(void) {
	mt6768_trace("[UBOOT] board_init reached (past relocation)\n");
	return 0;
}

/* One line per lk reservation, short enough to photograph off the screen. */
static void mt6768_print_lk_resv(void)
{
	const void *lk = (const void *)get_prev_bl_fdt_addr();
	int resv, sub;
	fdt_addr_t base;
	fdt_size_t size;

	if (!lk || fdt_check_header(lk)) {
		printf("lk FDT: none at %p\n", lk);
		return;
	}
	resv = fdt_path_offset(lk, "/reserved-memory");
	printf("lk FDT %p reserved-memory:\n", lk);
	fdt_for_each_subnode(sub, lk, resv) {
		base = fdtdec_get_addr_size_auto_parent(lk, resv, sub, "reg",
							0, &size, false);
		if (base != FDT_ADDR_T_NONE)
			printf(" %09llx %09llx %s\n", (u64)base, (u64)size,
			       fdt_get_name(lk, sub, NULL));
	}
}

int board_late_init(void)
{
	struct udevice *dev;

	/* Trace independent of the console: if this line shows up and the
	 * others do not, U-Boot is still running and only the console is
	 * broken.
	 */
	mt6768_trace("[UBOOT] board_late_init reached\n");

	/*
	 * Patch RAM into the original dtb right here, not waiting for booti.
	 * See the comment on mt6768_fixup_payload_dtb().
	 */
	mt6768_fixup_payload_dtb();

	const char *cmdline = get_cmdline();
	char serial[48];
	int ret;

	/*
	 * Trigger MUSB probe.
	 *
	 * Do not treat a failure as fatal: the node in the dtsi declares
	 * compatible = "mediatek,mt6768-musb" while the defconfig enables
	 * CONFIG_USB_MTU3, so no driver matches and it returns -ENODEV.
	 * Returning that error stops initcall_run_r() dead and U-Boot never
	 * reaches the prompt - while everything else (MMC, console, watchdog)
	 * already runs.
	 */
	/*
	 * Put the console on the screen too. This device has no reachable UART,
	 * but lk has already brought the panel up and left a framebuffer at
	 * 0x7dcb0000, so simple-framebuffer can reuse it straight away - the
	 * same way postmarketOS draws directly into /dev/fb0.
	 *
	 * stdout-path in the device tree only takes a single destination, so it
	 * has to be set through the environment; CONFIG_CONSOLE_MUX allows
	 * several outputs at once.
	 */
	if (uclass_first_device_err(UCLASS_VIDEO, &dev) == 0) {
		env_set("stdout", "serial,vidconsole");
		env_set("stderr", "serial,vidconsole");

		/*
		 * U-Boot's banner is printed before the console moves to the
		 * screen, so it is not visible on the device. Print it again
		 * here to tell at a glance which build is running.
		 */
		/* screen curves at the edges: push down a few lines to clear */
		printf("\n\n\n\n\n");
		printf("==================================================\n");
		printf("  U-Boot on %s (MT6768)\n", (const char *)fdt_getprop(gd->fdt_blob, 0, "model", NULL));
		printf("  %s\n", version_string);
		printf("  DRAM : %llu MiB\n",
		       (unsigned long long)(gd->ram_size >> 20));
		printf("==================================================\n\n");

		/* Curved screen and fast-scrolling text: pause to keep up. */
		mdelay(4000);
	} else {
		printf("%s: no video device found\n", __func__);
	}

	if (IS_ENABLED(CONFIG_MOTOROLA_LAMUC))
		mt6768_print_lk_resv();

	ret = uclass_get_device(UCLASS_USB_GADGET_GENERIC, 0, &dev);
	if (ret)
		printf("%s: no USB device found (err: %d), skipping\n",
		       __func__, ret);


#ifdef CONFIG_POWER
	ret = uclass_get_device_by_driver(UCLASS_PMIC, DM_DRIVER_GET(mtk_pwrap), &dev);
#endif

	/*
     * Set our custom kernel/FDT/ramdisk addresses
     * because we have CONFIG_ANDROID_BOOT_IMAGE_IGNORE_BLOB_ADDR enabled that fixes:
     * [    0.000000] [Firmware Bug]: Kernel image misaligned at boot, please fix your bootloader!
     * by ignoring boot.img's kernel address (LK expects kernel to be at 0x40080000, not aligned)
     * However, normally enabling this option and not setting these in env
     * causes bootm to malloc a buffer at 0x0 which makes it crash, we fix this.
     *
     * Fastboot buffer addr remains 0x45000000, we don't overlap with anything.
     *
     * XXX: maybe change this
     */
	env_set("kernel_addr_r", "40000000");
	env_set("fdt_addr_r", "41000000");
	env_set("ramdisk_addr_r", "42000000");
	env_set("fastboot_addr_r", "45000000");
	/* fastboot getvar stuff */
	env_set("platform", "mt6768");
	get_cmdline_option(cmdline, "androidboot.serialno=", serial,
			   sizeof(serial));
	if (serial[0] != '\0') {
		env_set("serial#", serial);
	} else {
		printf("%s: serialno not found in cmdline\n", __func__);
		env_set("serial#", "unknown");
	}

#ifdef CONFIG_XIAOMI_MERLIN
	env_set("board", "merlin");
#else
	env_set("board", "generic");
#endif

	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	const void *lk_fdt = get_prevbl_fdt_addr();
	int err;

	err = mt6768_fixup_ram(blob, "u-boot,ram-fixup-booti", 0);
	if (err)
		return err;

	/* Measurement: copy LK's real list to /chosen to read it from Linux. */
	mt6768_report_lk_fdt(blob, 0);

	/*
	 * Copy LK's reserved regions across. This MUST happen before Linux sees
	 * the full RAM size, otherwise the kernel allocates on top of firmware
	 * regions — exactly what corrupted the SD card.
	 */
	{
		int added = 0, skipped = 0, rsv = 0, rc;
		char msg[96];
		int cn;

		rc = mt6768_copy_lk_reserved(blob, &added, &skipped, &rsv,
					     0);

		snprintf(msg, sizeof(msg), "rc=%d add=%d skip=%d memrsv=%d/%d",
			 rc, added, skipped, rsv, fdt_num_mem_rsv(blob));
		printf("%s: copy reserved-memory from LK: %s\n", __func__, msg);

		cn = fdt_path_offset(blob, "/chosen");
		if (cn >= 0)
			fdt_setprop_string(blob, cn, "u-boot,lk-resv-copy",
					   msg);
	}


	/*
	 * Do NOT call fdt_copy_resv_mem_node() here any more — fully removed.
	 *
	 * This spot used to hold:
	 *     if (!lk_fdt || fdt_check_header(lk_fdt)) return 0;
	 *     fdt_set_totalsize(blob, fdt_totalsize(blob) + 4096);
	 *     fdt_copy_resv_mem_node(lk_fdt, blob);
	 * That was long-standing DEAD CODE because the env var prevbl_fdt_addr
	 * did not exist. In v4 CONFIG_SAVE_PREV_BL_FDT_ADDR was enabled to read
	 * LK's FDT, and that inadvertently **brought it back to life**.
	 * Consequences measured on the device:
	 *
	 *   - It copied all 18 regions with no overlap check, so
	 *     mblock-9-pstore (4d010000+e0000) FULLY COVERED ramoops@4d05f000.
	 *   - It only set no-map when LK's node had no-map, so mblock-9-pstore
	 *     got no-map while ramoops did not -> the kernel could not map the
	 *     ramoops region -> driver not registered, /sys/fs/pstore empty.
	 *   - Conversely mblock-16-ccci (256 MiB shared with the modem) did NOT
	 *     get no-map because LK does not mark it.
	 *
	 * Losing ramoops means losing the log channel when the device dies —
	 * the one tool most needed while trying to extend memory.
	 *
	 * Replaced by mt6768_copy_lk_reserved() above: it does check for
	 * overlaps, and sets no-map for every region it adds.
	 *
	 * LESSON: enabling a CONFIG can wake up dead code long forgotten.
	 * Remove dead code instead of leaving it lying around.
	 */
	(void)lk_fdt;

	return 0;
}
