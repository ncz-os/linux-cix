// SPDX-License-Identifier: GPL-2.0-only
/*
 * SCMI based Cix Energy Model driver
 *
 * Copyright 2024 Cix Technology Group Co., Ltd.All Rights Reserved.
 */

#include <linux/energy_model.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/scmi_protocol.h>
#include <linux/scmi_perf_domain.h>
#include <linux/slab.h>
#include <linux/units.h>
#include <linux/cix/cix_scmi_em.h>

static struct scmi_protocol_handle *ph;
static const struct scmi_perf_proto_ops *perf_ops;

struct cix_scmi_em_ctx {
	struct list_head node;
	struct device *dev;
	struct device *perf_dev;
};

static DEFINE_MUTEX(cix_scmi_em_lock);
static LIST_HEAD(cix_scmi_em_list);

static struct cix_scmi_em_ctx *cix_scmi_em_find_ctx(struct device *dev)
{
	struct cix_scmi_em_ctx *ctx;

	lockdep_assert_held(&cix_scmi_em_lock);

	list_for_each_entry(ctx, &cix_scmi_em_list, node) {
		if (ctx->dev == dev)
			return ctx;
	}

	return NULL;
}

static void cix_scmi_em_release_ctx(void *data)
{
	struct cix_scmi_em_ctx *ctx = data;

	mutex_lock(&cix_scmi_em_lock);
	list_del(&ctx->node);
	mutex_unlock(&cix_scmi_em_lock);
}

static struct device *cix_scmi_em_get_perf_dev(struct device *dev)
{
	struct cix_scmi_em_ctx *ctx;
	struct device *perf_dev = NULL;

	mutex_lock(&cix_scmi_em_lock);
	ctx = cix_scmi_em_find_ctx(dev);
	if (ctx)
		perf_dev = ctx->perf_dev;
	mutex_unlock(&cix_scmi_em_lock);

	return perf_dev;
}

static int __maybe_unused
cix_scmi_get_em_power(struct device *dev, unsigned long *power,
		   unsigned long *KHz)
{
	struct device *perf_dev = cix_scmi_em_get_perf_dev(dev);
	enum scmi_power_scale power_scale;
	unsigned long Hz;
	int ret;

	if (!perf_dev)
		return -ENODEV;

	power_scale = scmi_perf_domain_power_scale(perf_dev);

	Hz = *KHz * 1000;
	ret = scmi_perf_domain_est_power(perf_dev, &Hz, power);
	if (ret)
		return ret;

	if (power_scale == SCMI_POWER_MILLIWATTS)
		*power *= MICROWATT_PER_MILLIWATT;

	*KHz = Hz / 1000;

	return 0;
}

int cix_scmi_register_em(struct device *dev, struct device *perf_dev)
{
	struct em_data_callback em_cb = EM_DATA_CB(cix_scmi_get_em_power);
	enum scmi_power_scale power_scale;
	struct cix_scmi_em_ctx *ctx;
	bool em_power_scale = false;
	int ret, nr_opp;

	if (!dev || IS_ERR_OR_NULL(perf_dev))
		return -EINVAL;

	mutex_lock(&cix_scmi_em_lock);
	if (cix_scmi_em_find_ctx(dev)) {
		mutex_unlock(&cix_scmi_em_lock);
		return 0;
	}
	mutex_unlock(&cix_scmi_em_lock);

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
	ctx->perf_dev = perf_dev;
	power_scale = scmi_perf_domain_power_scale(ctx->perf_dev);

	if (power_scale == SCMI_POWER_MILLIWATTS ||
	    power_scale == SCMI_POWER_MICROWATTS) {
		em_power_scale = true;
	} else {
		dev_err(dev, "SCMI perf domain does not report real power units\n");
		ret = -EINVAL;
		goto err_free_ctx;
	}

	mutex_lock(&cix_scmi_em_lock);
	if (cix_scmi_em_find_ctx(dev)) {
		mutex_unlock(&cix_scmi_em_lock);
		ret = 0;
		goto err_free_ctx;
	}
	list_add(&ctx->node, &cix_scmi_em_list);
	mutex_unlock(&cix_scmi_em_lock);

	ret = devm_add_action_or_reset(dev, cix_scmi_em_release_ctx, ctx);
	if (ret)
		return ret;

	nr_opp = dev_pm_opp_get_opp_count(dev);
	if (nr_opp <= 0) {
		dev_err(dev, "Failed to get OPP counts\n");
		ret = -EINVAL;
		goto err_release_ctx;
	}

	dev_info(dev, "cix EM: pre-register em_pd=%p nr_opp=%d uW=%d\n",
		 dev->em_pd, nr_opp, em_power_scale);
	ret = em_dev_register_perf_domain(dev, nr_opp, &em_cb, NULL, em_power_scale);
	if (ret) {
		dev_dbg(dev, "Couldn't register Energy Model %d\n", ret);
		goto err_release_ctx;
	}

	return ret;

err_release_ctx:
	devm_remove_action(dev, cix_scmi_em_release_ctx, ctx);
	cix_scmi_em_release_ctx(ctx);
	return ret;

err_free_ctx:
	return ret;
}
EXPORT_SYMBOL_GPL(cix_scmi_register_em);

int cix_scmi_em_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle;

	handle = sdev->handle;

	if (!handle)
		return -ENODEV;

	perf_ops = handle->devm_protocol_get(sdev, SCMI_PROTOCOL_PERF, &ph);
	if (IS_ERR(perf_ops))
		return PTR_ERR(perf_ops);

	return 0;
}

static void cix_scmi_em_remove(struct scmi_device *sdev)
{
	/* Nothing need to be done now */
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_PERF, "cix_em_perf" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver cix_scmi_em_drv = {
	.name		= "cix-scmi-em",
	.probe		= cix_scmi_em_probe,
	.remove		= cix_scmi_em_remove,
	.id_table	= scmi_id_table,
};
module_scmi_driver(cix_scmi_em_drv);

MODULE_AUTHOR("Cixtech,Inc.");
MODULE_DESCRIPTION("CIX SCMI Energy Model interface driver");
MODULE_LICENSE("GPL v2");
