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
#include <linux/string.h>
#include <acpi/acpi_bus.h>

#include "internal.h"

/* SCMI parent: match children of CIXHA006 (SCMI controller) by parent HID */
static const struct acpi_device_id cix_scmi_parent_ids[] = {
	{ "CIXHA006", 0 },
	{ }
};

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

static bool acpi_name_is(struct acpi_device *adev, const char *name)
{
	struct acpi_buffer buf;
	char node_name[ACPI_NAMESEG_SIZE + 1] = { };

	buf.length = sizeof(node_name);
	buf.pointer = node_name;
	if (ACPI_FAILURE(acpi_get_name(adev->handle, ACPI_SINGLE_NAME, &buf)))
		return false;

	return !strcmp(node_name, name);
}

bool acpi_sta_override_firmware_quirk(struct acpi_device *adev,
				unsigned long long *status)
{
	struct acpi_device *parent = acpi_dev_parent(adev);

	/*
	 * NPU core devices (CRE0/CRE1/CRE2 under CIXH4000/NPU0):
	 * _STA=0 prevents full child binding.  Scope by parent HID and
	 * child ACPI name, not parent alone.
	 */
	if (parent && !acpi_match_device_ids(parent, cix_npu_parent_ids) &&
	    (acpi_name_is(adev, "CRE0") ||
	     acpi_name_is(adev, "CRE1") ||
	     acpi_name_is(adev, "CRE2"))) {
		*status = ACPI_STA_DEFAULT;
		return true;
	}

	/*
	 * SCMI protocol children under CIXHA006 are CIXHA008 (DVFS) and
	 * CIXHA009 (CLKS).  Child HID can be unavailable while _STA=0,
	 * so scope by parent HID plus child ACPI name.
	 */
	if (parent) {
		bool scmi_parent = !acpi_match_device_ids(parent, cix_scmi_parent_ids);
		const char *parent_uid = acpi_device_uid(parent);
		const char *child_uid = acpi_device_uid(adev);
		bool scmi_uid0 = !parent_uid || !strcmp(parent_uid, "0");
		bool scmi_child_uid0 = !child_uid || !strcmp(child_uid, "0");
		bool scmi_child_hid = !acpi_match_device_ids(adev, cix_scmi_proto_sta_ids);
		bool scmi_child_name = acpi_name_is(adev, "DVFS") ||
			acpi_name_is(adev, "CLKS");

		if (scmi_parent && scmi_uid0 && scmi_child_uid0 &&
		    (scmi_child_hid || scmi_child_name)) {
			*status = ACPI_STA_DEFAULT;
			return true;
		}
	}

	return false;
}
