// SPDX-License-Identifier: GPL-2.0-only
/*
 * ACPI _STA overrides for CIX BIOS v1.0 firmware bugs.
 *
 * CIX BIOS v1.0 leaves certain devices at _STA 0 so the ACPI bus scan
 * skips full enumeration.  Key quirk: children with _STA=0 are created
 * as _ADR-matched acpi_device structs WITHOUT _HID evaluation, so
 * acpi_match_device_ids() cannot be used on the child directly.
 * Match on the PARENT device HID via acpi_dev_parent() instead.
 *
 * Devices affected:
 *   CIXHA008, CIXHA009 (SCMI protocol, children of CIXHA006, uid=0)
 *   CRE0-2 (NPU cores, children of CIXH4000/NPU0, all uids)
 */

#include <linux/acpi.h>
#include <acpi/acpi_bus.h>

#include "internal.h"

/* SCMI protocol: match the CHILD device directly (HID is populated) */
static const struct acpi_device_id cix_scmi_proto_sta_ids[] = {
	{ "CIXHA008", 0 },
	{ "CIXHA009", 0 },
	{ }
};

/* NPU parent: match children of CIXH4000 (NPU0) by PARENT HID */
static const struct acpi_device_id cix_npu_parent_ids[] = {
	{ "CIXH4000", 0 },
	{ }
};

bool acpi_sta_override_firmware_quirk(struct acpi_device *adev,
				      unsigned long long *status)
{
	struct acpi_device *parent = acpi_dev_parent(adev);

	/*
	 * NPU core devices (CRE0/CRE1/CRE2 under CIXH4000/NPU0):
	 * _STA=0 prevents _HID from being evaluated on the child, so
	 * match by parent HID via acpi_dev_parent() instead.
	 */
	if (parent && !acpi_match_device_ids(parent, cix_npu_parent_ids)) {
		*status = ACPI_STA_DEFAULT;
		return true;
	}

	/* SCMI protocol devices: child HID is populated, uid=0 only */
	if (acpi_match_device_ids(adev, cix_scmi_proto_sta_ids))
		return false;

	if (!acpi_dev_uid_match(adev, 0))
		return false;

	*status = ACPI_STA_DEFAULT;
	return true;
}
