/*
 * SPDX-License-Identifier: GPL-2.0+
 *
 * Add CIX SKY1 SoC Version driver
 *
 */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/acpi.h>

enum {
	DENY_PCIE = 0,
	DENY_USB,
	DENY_SCMI_CLKS,
	DENY_MAX,
};

static bool pcie_pnp_en = false;
static bool usb_pnp_en = false;
static bool acpi_scmi_en = false;

static int __init pcie_pnp_update(char *str)
{
	pcie_pnp_en = true;

	return 0;
}
early_param("pcie_pnp_en", pcie_pnp_update);

static int __init usb_pnp_update(char *str)
{
	usb_pnp_en = true;

	return 0;
}
early_param("usb_pnp_en", usb_pnp_update);

static int __init parse_acpi_scmi_support(char *arg)
{
	if (!arg)
		return -EINVAL;

	if (strcmp(arg, "off") == 0)
		acpi_scmi_en = false;
	else if (strcmp(arg, "on") == 0)
		acpi_scmi_en = true;
	else
		return -EINVAL;

	return 0;
}
early_param("acpi_scmi_en", parse_acpi_scmi_support);

static struct acpi_device_id pcie_pnp_deny_ids[] = {
	/* override PCIE acpi scan handler */
	{"PNP0A03", 0}, /* PCIE */
	{"PNP0C0F", 0}, /* PCIE Links */
	{"", 0},
};

static struct acpi_device_id usb_pnp_deny_ids[] = {
	{"PNP0D10", 0},
	{"", 0},
};

static struct acpi_device_id pcie_cdns_deny_ids[] = {
	{"CIXH2020", 0}, /* pcie rc */
	{"CIXH2021", 0}, /* pcie ep */
	{"CIXH2023", 0}, /* pcie phy */
	{"", 0},
};

static struct acpi_device_id usb_cdns_deny_ids[] = {
	{"CIXH2030", 0}, /* sky1 usbssp */
	{"CIXH2031", 0}, /* cdns usbssp */
	{"CIXH2032", 0}, /* usb 2.0 phy */
	/* CIXH2033 is combo phy, also used by dp */
	{"CIXH2034", 0}, /* usb 3.0 phy */
	{"", 0},
};

static struct acpi_device_id acpi_clks_deny_ids[] = {
	{"CIXHA010", 0},
	{"", 0},
};

static struct acpi_device_id scmi_clks_deny_ids[] = {
	{"CIXHA009", 0},
	{"", 0},
};

static int acpi_device_deny(struct acpi_device *adev,
			     const struct acpi_device_id *not_used)
{
	dev_dbg(&adev->dev, "acpi disable dev[%s]\n", dev_name(&adev->dev));

	return -1; /* return -1 to block platform device enumeration */
}

static struct acpi_scan_handler acpi_deny_handler[DENY_MAX] = {
	{
		.ids = pcie_pnp_deny_ids,
		.attach = acpi_device_deny,
	},
	{
		.ids = usb_pnp_deny_ids,
		.attach = acpi_device_deny,
	}
};

/*
 * True only on real Sky1 firmware.
 *
 * The deny handlers below exist to stop the GENERIC ACPI enumerators from
 * claiming devices that the CIX vendor drivers must own.  That is correct on
 * Sky1 and actively harmful anywhere else: DENY_PCIE blocks PNP0A03, the
 * standard PCI root bridge, so on any non-Sky1 ACPI machine the PCI bus is
 * never enumerated at all -- no host controllers, no storage, no network.
 *
 * Measured under QEMU virt (MCFG OEM "BOCHS"): with the deny active the kernel
 * reported "ACPI: MCFG table detected" and then nothing, /sys/bus/pci/devices
 * was empty and /proc/partitions listed no devices whatsoever.  With the deny
 * skipped the same kernel brings up "ACPI: PCI Root Bridge [PCI0]", xhci_hcd,
 * virtio_blk and the USB medium.
 *
 * Detection matches pci-sky1-acpi.c, which already guards its own PNP0A08
 * block the same way.
 */
static bool sky1_firmware_detected(void)
{
	struct acpi_table_header *header;
	bool detected;

	if (acpi_disabled)
		return false;

	if (ACPI_FAILURE(acpi_get_table(ACPI_SIG_MCFG, 0, &header)))
		return false;

	detected = !memcmp(header->oem_id, "CIXTEK", 6) &&
		   !memcmp(header->oem_table_id, "SKY1EDK2", 8);
	acpi_put_table(header);

	return detected;
}

static void device_deny_id_init(void)
{
	int i;

	if (!sky1_firmware_detected()) {
		pr_info("cix-acpi-plat: not Sky1 firmware, leaving generic ACPI enumeration alone\n");
		return;
	}

	if (pcie_pnp_en)
		acpi_deny_handler[DENY_PCIE].ids = pcie_cdns_deny_ids;

	if (usb_pnp_en)
		acpi_deny_handler[DENY_USB].ids = usb_cdns_deny_ids;

	if (acpi_scmi_en)
		acpi_deny_handler[DENY_SCMI_CLKS].ids = acpi_clks_deny_ids;
	else
		acpi_deny_handler[DENY_SCMI_CLKS].ids = scmi_clks_deny_ids;

	for(i = 0; i < DENY_MAX; i++)
		acpi_scan_add_handler(&acpi_deny_handler[i]);
}

int cix_acpi_plat_arch_init(void)
{
	device_deny_id_init();

	return 0;
}
arch_initcall(cix_acpi_plat_arch_init);
