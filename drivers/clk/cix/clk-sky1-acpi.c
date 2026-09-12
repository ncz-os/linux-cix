// SPDX-License-Identifier: GPL-2.0
/*
 * CIX Sky1 ACPI clock lookup table (CLKT) driver
 *
 * Parses vendor CLKT tables in the ACPI namespace to bridge
 * SCMI-provided clocks to consumer devices via clkdev lookups.
 *
 * The vendor DSDT defines per-device CLKT Name objects:
 *   Name (CLKT, Package (N) {
 *       Package (3) { clock_id, con_name, consumer_ref },
 *       ...
 *   })
 *
 * This driver walks the ACPI namespace, evaluates each CLKT,
 * looks up the corresponding SCMI clock (registered by clk-scmi
 * as "scmi-clk-<id>"), and creates clkdev entries so consumer
 * drivers find clocks via standard clk_get().
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/device/bus.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/scmi_protocol.h>
#include <linux/string.h>

extern const struct bus_type scmi_bus_type;

struct sky1_acpi_clk_map {
	struct list_head node;
	u64 clock_id;
	const char *con_id;
	const char *dev_id;
};

struct sky1_acpi_clk {
	struct device *dev;
	struct notifier_block scmi_nb;
	struct mutex lock;
	struct list_head mapped_list;
	int mapped;
	int pending;
	int error;
};

static void sky1_clkdev_drop(void *data)
{
	clkdev_drop(data);
}

static void sky1_scmi_bus_notifier_unregister(void *data)
{
	bus_unregister_notifier(&scmi_bus_type, data);
}

static bool sky1_clkt_entry_mapped(struct sky1_acpi_clk *priv, u64 clock_id,
				   const char *con_id, const char *dev_id)
{
	struct sky1_acpi_clk_map *map;

	list_for_each_entry(map, &priv->mapped_list, node) {
		if (map->clock_id == clock_id &&
		    !strcmp(map->dev_id, dev_id) &&
		    !strcmp(map->con_id ?: "", con_id ?: ""))
			return true;
	}

	return false;
}

static struct sky1_acpi_clk_map *
sky1_clkt_alloc_entry(struct sky1_acpi_clk *priv, u64 clock_id,
		      const char *con_id, const char *dev_id)
{
	struct sky1_acpi_clk_map *map;

	map = devm_kzalloc(priv->dev, sizeof(*map), GFP_KERNEL);
	if (!map)
		return ERR_PTR(-ENOMEM);

	map->dev_id = devm_kstrdup(priv->dev, dev_id, GFP_KERNEL);
	if (!map->dev_id)
		return ERR_PTR(-ENOMEM);

	if (con_id) {
		map->con_id = devm_kstrdup(priv->dev, con_id, GFP_KERNEL);
		if (!map->con_id)
			return ERR_PTR(-ENOMEM);
	}

	map->clock_id = clock_id;

	return map;
}

static int sky1_parse_clkt_entry(struct sky1_acpi_clk *priv,
				 const char *consumer_name,
				 union acpi_object *entry)
{
	union acpi_object *elems;
	u64 clock_id;
	const char *con_id;
	char scmi_id[20];
	struct clk *clk;
	struct clk_lookup *cl;
	struct sky1_acpi_clk_map *map;
	int ret;

	if (entry->type != ACPI_TYPE_PACKAGE || entry->package.count < 2)
		return -EINVAL;

	elems = entry->package.elements;

	/* Element 0: SCMI clock ID (integer) */
	if (elems[0].type != ACPI_TYPE_INTEGER)
		return -EINVAL;
	clock_id = elems[0].integer.value;

	/* Element 1: connection name (string, empty = unnamed) */
	if (elems[1].type != ACPI_TYPE_STRING)
		return -EINVAL;
	con_id = elems[1].string.pointer;
	if (con_id[0] == '\0')
		con_id = NULL;

	if (sky1_clkt_entry_mapped(priv, clock_id, con_id, consumer_name))
		return 0;

	/* Look up the SCMI clock by its clkdev registration name */
	snprintf(scmi_id, sizeof(scmi_id), "scmi-clk-%llu", clock_id);
	clk = clk_get(NULL, scmi_id);
	if (IS_ERR(clk)) {
		dev_dbg(priv->dev, "SCMI clock %llu not available for %s\n",
			clock_id, consumer_name);
		return PTR_ERR(clk);
	}

	map = sky1_clkt_alloc_entry(priv, clock_id, con_id, consumer_name);
	if (IS_ERR(map)) {
		clk_put(clk);
		return PTR_ERR(map);
	}

	/* Register consumer lookup */
	cl = clkdev_create(clk, con_id, "%s", consumer_name);
	clk_put(clk);
	if (!cl) {
		dev_warn(priv->dev,
			 "Failed to create clkdev for %s clock %llu\n",
			 consumer_name, clock_id);
		return -ENOMEM;
	}

	/* Auto-cleanup on device removal */
	ret = devm_add_action_or_reset(priv->dev, sky1_clkdev_drop, cl);
	if (ret)
		return ret;

	list_add_tail(&map->node, &priv->mapped_list);
	dev_dbg(priv->dev, "Mapped SCMI clock %llu -> %s:%s\n",
		clock_id, consumer_name, con_id ?: "(unnamed)");
	priv->mapped++;

	return 0;
}

static acpi_status sky1_clkt_walk_cb(acpi_handle handle, u32 level,
				     void *context, void **ret_val)
{
	struct sky1_acpi_clk *priv = context;
	struct acpi_device *adev;
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	const char *consumer_name;
	acpi_status status;
	int ret;
	int i;

	status = acpi_evaluate_object(handle, "CLKT", NULL, &buf);
	if (ACPI_FAILURE(status))
		return AE_OK;

	obj = buf.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE || obj->package.count == 0)
		goto out;

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev)
		goto out;

	for (i = 0; i < obj->package.count; i++) {
		union acpi_object *e = &obj->package.elements[i];
		union acpi_object *ref;

		/*
		 * Element 2 (if present) is the actual consumer device
		 * reference.  Use it instead of the CLKT-owning device.
		 * Most entries are self-references (consumer == owner),
		 * but some cross-reference a different device (e.g.,
		 * DMA1's CLKT registers a clock for DMA0).
		 */
		if (e->type == ACPI_TYPE_PACKAGE &&
		    e->package.count >= 3) {
			struct acpi_device *cdev;

			ref = &e->package.elements[2];
			if (ref->type == ACPI_TYPE_LOCAL_REFERENCE) {
				cdev = acpi_fetch_acpi_dev(ref->reference.handle);
				if (cdev) {
					consumer_name = acpi_dev_name(cdev);
					goto parse;
				}
			}
		}
		consumer_name = acpi_dev_name(adev);
parse:
		ret = sky1_parse_clkt_entry(priv, consumer_name, e);
		if (ret == -ENOENT || ret == -EPROBE_DEFER)
			priv->pending++;
		else if (ret && !priv->error)
			priv->error = ret;
	}

out:
	kfree(buf.pointer);
	return AE_OK;
}

static int sky1_clkt_walk(struct sky1_acpi_clk *priv)
{
	acpi_status status;

	priv->pending = 0;
	priv->error = 0;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				     ACPI_UINT32_MAX, sky1_clkt_walk_cb,
				     NULL, priv, NULL);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	return priv->error;
}

static int sky1_scmi_bus_notify(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct sky1_acpi_clk *priv =
		container_of(nb, struct sky1_acpi_clk, scmi_nb);
	struct scmi_device *sdev;
	struct device *dev = data;
	int old_mapped;
	int ret;

	if (action != BUS_NOTIFY_BOUND_DRIVER || !dev)
		return NOTIFY_DONE;

	sdev = to_scmi_dev(dev);
	if (sdev->protocol_id != SCMI_PROTOCOL_CLOCK ||
	    strcmp(sdev->name, "clocks"))
		return NOTIFY_DONE;

	mutex_lock(&priv->lock);
	old_mapped = priv->mapped;
	ret = sky1_clkt_walk(priv);
	if (ret)
		dev_warn(priv->dev, "ACPI CLKT remap failed: %d\n", ret);
	else if (priv->mapped != old_mapped)
		dev_info(priv->dev, "Mapped %d additional ACPI clock lookup entries\n",
			 priv->mapped - old_mapped);
	mutex_unlock(&priv->lock);

	return NOTIFY_OK;
}

static int sky1_acpi_clk_probe(struct platform_device *pdev)
{
	struct sky1_acpi_clk *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	mutex_init(&priv->lock);
	INIT_LIST_HEAD(&priv->mapped_list);
	priv->scmi_nb.notifier_call = sky1_scmi_bus_notify;

	ret = bus_register_notifier(&scmi_bus_type, &priv->scmi_nb);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register SCMI bus notifier\n");

	ret = devm_add_action_or_reset(&pdev->dev,
				       sky1_scmi_bus_notifier_unregister,
				       &priv->scmi_nb);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);
	ret = sky1_clkt_walk(priv);
	mutex_unlock(&priv->lock);
	if (ret == -ENODEV)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "ACPI namespace walk failed\n");

	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Invalid ACPI CLKT entry\n");

	/*
	 * Nothing mapped at all: no clkdevs exist yet, so deferring here
	 * cannot cause the teardown/re-walk storm that gating on *partial*
	 * maps did. A retry re-registers the notifier, so a late clocks
	 * device is still not missed.
	 */
	if (priv->mapped == 0)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "No SCMI clocks mapped yet\n");

	if (priv->pending)
		dev_dbg(&pdev->dev,
			"%d ACPI CLKT entries still pending, dependent consumers will retry independently\n",
			priv->pending);

	dev_info(&pdev->dev, "Mapped %d ACPI clock lookup entries\n",
		 priv->mapped);
	return 0;
}

static const struct acpi_device_id sky1_acpi_clk_ids[] = {
	{ "CIXHA010", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, sky1_acpi_clk_ids);

static struct platform_driver sky1_acpi_clk_driver = {
	.driver = {
		.name = "clk-sky1-acpi",
		.acpi_match_table = sky1_acpi_clk_ids,
	},
	.probe = sky1_acpi_clk_probe,
};
module_platform_driver(sky1_acpi_clk_driver);

MODULE_DESCRIPTION("CIX Sky1 ACPI clock lookup table driver");
MODULE_LICENSE("GPL");
