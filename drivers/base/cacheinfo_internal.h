/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DRIVERS_BASE_CACHEINFO_INTERNAL_H
#define _DRIVERS_BASE_CACHEINFO_INTERNAL_H

#include <linux/cacheinfo.h>

bool cacheinfo_cache_leaves_are_shared(const struct cacheinfo *this_leaf,
				       const struct cacheinfo *sib_leaf);

#endif /* _DRIVERS_BASE_CACHEINFO_INTERNAL_H */
