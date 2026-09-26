// SPDX-License-Identifier: GPL-2.0
/*
 * NCZ resume beacon: append suspend/resume progress tags into the
 * firmware-reserved ramoops console zone (CIX Sky1) with explicit
 * dcache clean so records survive a watchdog reset.  Self-contained
 * resume-side companion to the unwired 9021 boot-beacon helper.
 */
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ncz-resume-beacon.h>
#include <asm/cacheflush.h>

#define PRZ_SIG 0x43474244u
#define MAP_LEN (SZ_64K)

struct prz_hdr {
	u32 sig;
	u32 start;
	u32 size;
};

static u32 ncz_cursor;
static bool ncz_inited;
static void *ncz_late_base;

static void ncz_flush(void *addr, size_t len)
{
	dcache_clean_poc((unsigned long)addr, (unsigned long)addr + len);
}

static void *ncz_map(void)
{
	if (slab_is_available()) {
		if (!ncz_late_base)
			ncz_late_base = memremap(CONFIG_NCZ_BEACON_PHYS,
						 MAP_LEN, MEMREMAP_WB);
		return ncz_late_base;
	}
	return NULL;
}

void ncz_beacon(const char *tag)
{
	struct prz_hdr *h;
	void *base;
	char line[96];
	int len;

	/*
	 * All beacon call sites run after slab is up (the deepest site
	 * is __cpu_suspend_exit(), which is reached via cpu_resume()
	 * returning into the cpu_suspend() trampoline -- slab has been
	 * live for many milliseconds at that point).  We therefore only
	 * support the late memremap path; refusing the call rather than
	 * falling back to early_memremap keeps the helper simple and
	 * means a beacon taken in truly pre-slab context (which none of
	 * the resume sites are) is silently dropped instead of crashing
	 * the resume path on a NULL deref.
	 */
	if (!slab_is_available())
		return;

	base = ncz_map();
	if (!base)
		return;

	h = base;
	if (!ncz_inited) {
		h->sig = PRZ_SIG;
		h->start = 0;
		h->size = 0;
		ncz_cursor = 0;
		ncz_inited = true;
		ncz_flush(h, sizeof(*h));
	}

	len = scnprintf(line, sizeof(line), "NCZBEACON %s\n", tag);
	if (ncz_cursor + len < MAP_LEN - sizeof(*h)) {
		char *dst = (char *)base + sizeof(*h) + ncz_cursor;

		memcpy(dst, line, len);
		ncz_flush(dst, len);
		ncz_cursor += len;
		h->start = ncz_cursor;
		h->size = ncz_cursor;
		ncz_flush(h, sizeof(*h));
	}
}
EXPORT_SYMBOL(ncz_beacon);
