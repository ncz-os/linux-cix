/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NCZ_RESUME_BEACON_H
#define _LINUX_NCZ_RESUME_BEACON_H

#ifdef CONFIG_NCZ_RESUME_BEACON
void ncz_beacon(const char *tag);
#else
static inline void ncz_beacon(const char *tag)
{
	(void)tag;
}
#endif

#endif
