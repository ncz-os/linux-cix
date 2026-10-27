/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/dmi.h>
#include <linux/string.h>
#include "cix_dmi.h"

static char *cpu_name = "Version";
static bool get_cpu_flag = false;

static void get_cpuname_by_dmi(const struct dmi_header *dm, void *data)
{
        const char *bp;

        if (!dm)
                return;
        if (dm->type != DMI_ENTRY_PROCESSOR)
                return;

        /*
         * dmi_string_nosave() performs the same walk this file used to
         * open-code (offset 0x10 holds the processor-version string index),
         * but it is the helper the rest of the kernel relies on and it
         * bounds the walk by the DMI record.  It returns "" for an empty,
         * whitespace-only or absent string, so no separate space scan or
         * strlen() over untrusted table data is needed here.
         */
        bp = dmi_string_nosave(dm, ((const u8 *)dm)[0x10]);
        if (!bp || !bp[0])
                return;

        cpu_name = dmi_alloc(strlen(bp) + 1);
        if (!cpu_name)
                return;

        strscpy(cpu_name, bp, strlen(bp) + 1);
        get_cpu_flag = true;
}

char *get_cpu_name(void)
{
        if(!get_cpu_flag)
                dmi_walk(get_cpuname_by_dmi, cpu_name);

        if(cpu_name == NULL) {
                /* Some BIOS don't support getting CPU name from DMI  */
                return "UNKNOWN-CPU";
        }

        return cpu_name;
}
