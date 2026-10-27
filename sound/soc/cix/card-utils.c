// SPDX-License-Identifier: GPL-2.0
// Copyright 2024 Cix Technology Group Co., Ltd.
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include "card-utils.h"
#include <sound/jack.h>
#include <sound/hdmi-codec.h>

#define SKY1_AUDSS_CRU_INFO_MCLK		0x70
#define SKY1_AUDSS_CRU_INFO_MCLK_DIV_OFF(x)	(10 + (3 * (x)))
#define SKY1_AUDSS_CRU_INFO_MCLK_DIV_MASK(x)	GENMASK((12 + (3 * (x))), (10 + (3 * (x))))

#define SKY1_AUDSS_DP_AUDIO_COUNT		5
#define SKY1_AUDSS_DP_AUDIO_OFFSET		5

static const char *mclk_pll_names[AUDIO_CLK_NUM] = {
	[AUDIO_CLK0] = "audio_clk0",
	[AUDIO_CLK2] = "audio_clk2",
};

static int cix_jack_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct dai_link_info *link_info =
		(struct dai_link_info *)((priv->link_info + rtd->id));
	struct device *dev = rtd->card->dev;
	int ret, jack[JACK_CNT], cnt, i;

	if (!link_info->jack_det_mask)
		return 0;

	cnt = 0;
	if (link_info->jack_det_mask & JACK_MASK_DPIN) {
		jack[cnt] = JACK_DPIN;
		cnt++;
	}

	if (link_info->jack_det_mask & JACK_MASK_DPOUT) {
		jack[cnt] = JACK_DPOUT;
		cnt++;
	}

	if (link_info->jack_det_mask & JACK_MASK_HP) {
		jack[cnt] = JACK_HP;
		cnt++;
	}

	for (i = 0; i < cnt; i++) {
		ret = snd_soc_card_jack_new(rtd->card,
				link_info->jack_pin[jack[i]].pin,
				link_info->jack_pin[jack[i]].mask,
				&link_info->jack[jack[i]]);
		if (ret) {
			dev_err(dev, "can't new jack:%d, %d\n", i, ret);
			return ret;
		}
		dev_info(dev, "codec component %s\n", codec_dai->component->name);

		ret = snd_soc_component_set_jack(codec_dai->component,
						 &link_info->jack[jack[i]], NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static int cix_dailink_parsing_fmt(struct device_node *np,
				   struct device_node *codec_np,
				   unsigned int *fmt)
{
	struct device_node *bitclkmaster = NULL;
	struct device_node *framemaster = NULL;
	unsigned int dai_fmt;

	dai_fmt = snd_soc_daifmt_parse_format(np, NULL);
	snd_soc_daifmt_parse_clock_provider_as_phandle(np, NULL, &bitclkmaster,
						       &framemaster);
	if (bitclkmaster != framemaster) {
		pr_info("Must be the same bitclock and frame master\n");
		return -EINVAL;
	}
	if (bitclkmaster) {
		dai_fmt &= ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK;
		if (codec_np == bitclkmaster)
			dai_fmt |= SND_SOC_DAIFMT_CBP_CFP;
		else
			dai_fmt |= SND_SOC_DAIFMT_CBC_CFC;
	}
	of_node_put(bitclkmaster);
	of_node_put(framemaster);
	*fmt = dai_fmt;

	return 0;
}

/* only disabled dai-link status, not continue to parse */
static bool cix_dailink_status_check(struct device_node *np)
{
	const char *status;

	if (!of_property_read_string(np, "status", &status))
		return !strcmp(status, "okay") || !strcmp(status, "ok");

	return true;
}

static int dai_set_sysclk(struct snd_pcm_substream *substream,
			  struct snd_pcm_hw_params *params,
			  unsigned int mclk_fs)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct dai_link_info *link_info =
		(struct dai_link_info *)((priv->link_info + rtd->id));
	struct snd_soc_dai *codec_dai;
	unsigned int mclk, mclk_div, mclk_parent_rate, sample_rate;
	struct clk *mclk_parent;
	int ret, i;
	u32 val;

	if (!mclk_fs || !priv->cru_regmap)
		return 0;

	sample_rate = params_rate(params);

	dev_dbg(rtd->dev, "sample rate:%d, mclk-fs ratio:%d\n", sample_rate, mclk_fs);

	/* Adjust mclk_fs due to mclk design limitation */
	if (sample_rate % 8000 == 0) {
		if (sample_rate < 32000 || sample_rate > 192000) {
			dev_err(rtd->dev,
				"8khz_pll cannot satisfy mclk less than 32khz or large than 192khz");
			return -EINVAL;
		}

		if (sample_rate == 32000) {
			mclk_fs = 512;
			mclk_div = 3;
		} else if (sample_rate == 48000) {
			mclk_fs = 512;
			mclk_div = 2;
		} else if (sample_rate == 64000) {
			mclk_fs = 256;
			mclk_div = 3;
		} else if (sample_rate == 96000) {
			mclk_fs = 256;
			mclk_div = 2;
		} else if (sample_rate == 192000) {
			mclk_fs = 256;
			mclk_div = 0;
		} else {
			dev_err(rtd->dev, "Unsupported sample rate %u for 8kHz clock family\n",
				sample_rate);
			return -EINVAL;
		}

		mclk_parent = link_info->clks[AUDIO_CLK0];
	} else if (sample_rate % 11025 == 0) {
		if (sample_rate < 44100 || sample_rate > 176400) {
			dev_err(rtd->dev,
				"11.025khz_pll cannot satisfy mclk less than 44.1khz or large than 176.4khz");
			return -EINVAL;
		}

		if (sample_rate == 44100) {
			mclk_fs = 512;
			mclk_div = 2;
		} else if (sample_rate == 88200) {
			mclk_fs = 256;
			mclk_div = 2;
		} else if (sample_rate == 176400) {
			mclk_fs = 256;
			mclk_div = 0;
		} else {
			dev_err(rtd->dev, "Unsupported sample rate %u for 11.025kHz clock family\n",
				sample_rate);
			return -EINVAL;
		}

		mclk_parent = link_info->clks[AUDIO_CLK2];
	} else {
		dev_err(rtd->dev, "invalid sample rate");
		return -EINVAL;
	}

	mclk_parent_rate = clk_get_rate(mclk_parent);
	dev_dbg(rtd->dev, "mclk parent rate = %d\n", mclk_parent_rate);

	mclk = sample_rate * mclk_fs;

	dev_dbg(rtd->dev, "mclk-fs ratio:%d, mclk-div:%d, mclk freq:%d\n",
		 mclk_fs, mclk_div, mclk);

	/* for cpu dai */
	regmap_read(priv->cru_regmap, SKY1_AUDSS_CRU_INFO_MCLK, &val);
	val &= ~SKY1_AUDSS_CRU_INFO_MCLK_DIV_MASK(link_info->mclk_idx);
	val |= (mclk_div << SKY1_AUDSS_CRU_INFO_MCLK_DIV_OFF(link_info->mclk_idx));
	regmap_write(priv->cru_regmap, SKY1_AUDSS_CRU_INFO_MCLK, val);

	ret = clk_set_parent(link_info->clk_mclk, mclk_parent);
	if (ret) {
		dev_err(rtd->dev, "failed to set mclk parent\n");
		return ret;
	}

	/* for codec dai */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_sysclk(codec_dai, 0, mclk,
				SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP)
			return ret;
	}

	return 0;
}

static int dai_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct dai_link_info *link_info =
		(struct dai_link_info *)((priv->link_info + rtd->id));
	int ret;

	if (link_info->mclk_fs) {
		ret = clk_prepare_enable(link_info->clk_mclk);
		if (ret) {
			dev_err(rtd->dev, "failed to enable mclk\n");
			return ret;
		}

		link_info->mclk_enabled = true;
	}

	return 0;
}

static void dai_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct dai_link_info *link_info =
		(struct dai_link_info *)((priv->link_info + rtd->id));

	if (link_info->mclk_fs) {
		link_info->mclk_enabled = false;

		clk_disable_unprepare(link_info->clk_mclk);
	}
}

static int dai_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct dai_link_info *link_info =
		(struct dai_link_info *)((priv->link_info + rtd->id));

	if (link_info->mclk_fs)
		return dai_set_sysclk(substream, params, link_info->mclk_fs);

	return 0;
}

static struct snd_soc_ops cix_dailink_ops = {
	.startup = dai_startup,
	.hw_params = dai_hw_params,
	.shutdown = dai_shutdown,
};

static int cix_dailink_init(struct snd_soc_pcm_runtime *rtd)
{
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(rtd->card);
	struct device *dev = rtd->card->dev;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct dai_link_info *link_info =
		(struct dai_link_info *)((priv->link_info + rtd->id));
	struct snd_soc_dai_link *dai_link =
		(struct snd_soc_dai_link *)((priv)->card->dai_link + rtd->id);
	int ret, fmt;

	dev_dbg(dev, "%s, dai_fmt:0x%x\n", __func__, dai_link->dai_fmt);

	ret = snd_soc_dai_set_fmt(cpu_dai, dai_link->dai_fmt);
	if (ret && ret != -ENOTSUPP)
	      return ret;

	fmt = dai_link->dai_fmt & SND_SOC_DAIFMT_FORMAT_MASK;
	if (fmt == SND_SOC_DAIFMT_DSP_A || fmt == SND_SOC_DAIFMT_DSP_B) {
		dev_dbg(dev,
			"\ttdm tx mask:0x%x, rx mask:0x%x, slots:%d, slot width:%d\n",
			link_info->tx_mask, link_info->rx_mask,
			link_info->slots, link_info->slot_width);

		ret = snd_soc_dai_set_tdm_slot(cpu_dai, link_info->tx_mask,
				link_info->rx_mask, link_info->slots, link_info->slot_width);
		if (ret && ret != -ENOTSUPP)
			return ret;
	}

	return cix_jack_init(rtd);
}

static int cix_gpio_init(struct cix_asoc_card *priv)
{
	struct snd_soc_card *card = priv->card;
	struct device *dev = card->dev;
	int ret;

	priv->pdb0_gpiod = devm_gpiod_get_optional(dev, "pdb0", GPIOD_OUT_HIGH | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(priv->pdb0_gpiod)) {
		ret = PTR_ERR(priv->pdb0_gpiod);
		dev_err(dev, "failed to pdb gpio, ret: %d\n", ret);
		return ret;
	}

	priv->pdb1_gpiod = devm_gpiod_get_optional(dev, "pdb1", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->pdb1_gpiod)) {
		ret = PTR_ERR(priv->pdb1_gpiod);
		dev_err(dev, "failed to get amplifier gpio: %d\n", ret);
		return ret;
	}

	priv->pdb2_gpiod = devm_gpiod_get_optional(dev, "pdb2", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->pdb2_gpiod)) {
		ret = PTR_ERR(priv->pdb2_gpiod);
		dev_err(dev, "failed to get amplifier gpio: %d\n", ret);
		return ret;
	}

	priv->pdb3_gpiod = devm_gpiod_get_optional(dev, "pdb3", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->pdb3_gpiod)) {
		ret = PTR_ERR(priv->pdb3_gpiod);
		dev_err(dev, "failed to get amplifier gpio: %d\n", ret);
		return ret;
	}

	return 0;
}

static int cix_card_suspend_post(struct snd_soc_card *card)
{
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd;
	struct dai_link_info *link_info;

	for_each_card_rtds(card, rtd) {
		link_info = (struct dai_link_info *)((priv->link_info + rtd->id));

		if (link_info->mclk_enabled)
			clk_disable_unprepare(link_info->clk_mclk);
	}

	return 0;
}

static int cix_card_resume_pre(struct snd_soc_card *card)
{
	struct cix_asoc_card *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd;
	struct dai_link_info *link_info;
	int ret;

	for_each_card_rtds(card, rtd) {
		link_info = (struct dai_link_info *)((priv->link_info + rtd->id));

		if (link_info->mclk_enabled) {
			ret = clk_prepare_enable(link_info->clk_mclk);
			if (ret) {
				dev_err(rtd->dev, "failed to enable mclk\n");
				return ret;
			}
		}
	}

	return 0;
}

int cix_card_parse_of(struct cix_asoc_card *priv)
{
	struct snd_soc_card *card = priv->card;
	struct device *dev = card->dev;
	struct device_node *np;
	struct device_node *codec;
	struct device_node *cpu;
	struct snd_soc_dai_link *link;
	struct dai_link_info *link_info;
	struct of_phandle_args args;
	struct snd_soc_dai_link_component *comp_cpu;
	int i, ret, num_links, current_np;

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret) {
		dev_err(dev, "error parsing card name: %d\n", ret);
		return ret;
	}

	ret = cix_gpio_init(priv);
	if (ret) {
		dev_err(dev, "failed to init gpio: %d\n", ret);
		return ret;
	}

	priv->cru_regmap = syscon_regmap_lookup_by_phandle(dev->of_node, "cru-ctrl");
	if (PTR_ERR(priv->cru_regmap) == -ENODEV) {
		priv->cru_regmap = NULL;
	} else if (IS_ERR(priv->cru_regmap)) {
		return PTR_ERR(priv->cru_regmap);
	}

	num_links = of_get_child_count(dev->of_node);

	link = devm_kcalloc(dev, num_links, sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;
	link_info = devm_kcalloc(dev, num_links, sizeof(*link_info), GFP_KERNEL);
	if (!link_info)
		return -ENOMEM;

	link->trigger_start = SND_SOC_TRIGGER_ORDER_DEFAULT;
	link->trigger_stop = SND_SOC_TRIGGER_ORDER_LDC;

	card->num_links = num_links;
	card->dai_link = link;
	card->suspend_post = cix_card_suspend_post;
	card->resume_pre = cix_card_resume_pre;
	priv->link_info = link_info;

	current_np = 0;
	for_each_child_of_node(dev->of_node, np) {
		if (!cix_dailink_status_check(np)) {
			num_links--;
			card->num_links = num_links;
			continue;
		}

		comp_cpu = devm_kzalloc(dev, 2 * sizeof(*comp_cpu), GFP_KERNEL);
		if (!comp_cpu) {
			ret = -ENOMEM;
			goto err_put_np;
		}

		cpu = of_get_child_by_name(np, "cpu");
		if (!cpu) {
			dev_err(dev, "%s: can't find cpu device node\n", cpu->name);
			ret = -EINVAL;
			goto err_put_cpu;
		}

		ret = of_parse_phandle_with_args(cpu, "sound-dai",
						 "#sound-dai-cells", 0, &args);
		if (ret) {
			dev_err(dev, "%s: error getting cpu phandle\n", cpu->name);
			goto err_put_cpu;
		}

		link->cpus = &comp_cpu[0];
		link->num_cpus = 1;
		link->cpus->of_node = args.np;
		/* rtd->id indexes the compact link and link_info arrays. */
		link->id = current_np;

		link->platforms	= &comp_cpu[1];
		link->num_platforms = 1;
		link->platforms->of_node = link->cpus->of_node;

		dev_info(dev, "dai-link name:%s\n", np->name);

		ret = snd_soc_of_get_dai_name(cpu, &link->cpus->dai_name, 0);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				dev_err(card->dev, "%s: error getting cpu dai name: %d\n",
					link->name, ret);
			goto err_put_cpu;
		}
		dev_info(dev, "\tcpu dai name:%s\n", link->cpus->dai_name);

		codec = of_get_child_by_name(np, "codec");
		if (codec) {
			ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
			if (ret < 0) {
				if (ret != -EPROBE_DEFER)
					dev_err(card->dev, "%s: codec dai not found: %d\n",
						link->name, ret);
				goto err_put_codec;
			}
		} else {
			struct snd_soc_dai_link_component *comp_codec;

			comp_codec = devm_kzalloc(dev, sizeof(*comp_codec), GFP_KERNEL);
			if (!comp_codec) {
				ret = -ENOMEM;
				goto err_put_codec;
			}

			link->num_codecs = 1;
			link->codecs = comp_codec;
			link->codecs->dai_name = "snd-soc-dummy-dai";
			link->codecs->name = "snd-soc-dummy";
		}
		dev_info(dev, "\tcodec dai name:%s\n", link->codecs->dai_name );

		ret = cix_dailink_parsing_fmt(np, codec, &link->dai_fmt);
		if (ret)
			goto err_put_codec;

		of_property_read_u32(np, "mclk-fs", &link_info->mclk_fs);
		if (link_info->mclk_fs) {
			link_info->clk_mclk = devm_get_clk_from_child(dev, np, "mclk");
			if (IS_ERR(link_info->clk_mclk)) {
				dev_err(dev, "failed to get clk_mclk clock\n");
				return PTR_ERR(link_info->clk_mclk);
			}

			for (i = 0; i < AUDIO_CLK_NUM; i++) {
				link_info->clks[i] = devm_get_clk_from_child(dev, np, mclk_pll_names[i]);
				if (IS_ERR(link_info->clks[i])) {
					dev_err(dev, "failed to get clock %s\n", mclk_pll_names[i]);
					return PTR_ERR(link_info->clks[i]);
				}
			}

			ret = of_property_read_u8(np, "mclk-idx", &link_info->mclk_idx);
			if (ret) {
				dev_err(dev, "failed to get mclk-idx: %d", ret);
				return ret;
			}
		}

		ret = snd_soc_of_parse_tdm_slot(np, &link_info->tx_mask,
						&link_info->rx_mask,
						&link_info->slots,
						&link_info->slot_width);
		if (ret)
			goto err_put_codec;

		if (of_property_read_bool(np, "jack-det,dpin")) {
			link_info->jack_pin[JACK_DPIN].pin = np->name ? np->name : "jack-dpin";
			link_info->jack_pin[JACK_DPIN].mask = SND_JACK_LINEIN;
			link_info->jack_det_mask |= JACK_MASK_DPIN;
		}
		if (of_property_read_bool(np, "jack-det,dpout")) {
			char dp_str[32];

			snprintf(dp_str, sizeof(dp_str),
				"HDMI/DP,pcm=%d", current_np);

			link_info->jack_pin[JACK_DPOUT].pin =
				devm_kstrdup(dev, dp_str, GFP_KERNEL);
			if (!link_info->jack_pin[JACK_DPOUT].pin) {
				ret = -ENOMEM;
				goto err_put_codec;
			}
			link_info->jack_pin[JACK_DPOUT].mask = SND_JACK_LINEOUT;
			link_info->jack_det_mask |= JACK_MASK_DPOUT;
		}
		if (of_property_read_bool(np, "jack-det,hp")) {
			link_info->jack_pin[JACK_HP].pin = "Headset";
			link_info->jack_pin[JACK_HP].mask = SND_JACK_HEADSET;
			link_info->jack_det_mask |= JACK_MASK_HP;
		}

		dev_info(dev, "\t\tdai_fmt:0x%x\n", link->dai_fmt);
		dev_info(dev, "\t\tmclk_fs:%d\n", link_info->mclk_fs);
		dev_info(dev,
			"\t\ttdm tx mask:0x%x, rx mask:0x%x, slots:%d, slot width:%d\n",
			link_info->tx_mask, link_info->rx_mask,
			link_info->slots, link_info->slot_width);
		dev_info(dev, "\t\tjack_det_mask:0x%x\n",
			link_info->jack_det_mask);

		link->stream_name = np->name ? np->name : link->cpus->dai_name;
		link->name = np->name ? np->name : link->cpus->dai_name;
		link->ops = &cix_dailink_ops;
		link->init = cix_dailink_init;
		link->trigger_start = SND_SOC_TRIGGER_ORDER_DEFAULT;
		link->trigger_stop = SND_SOC_TRIGGER_ORDER_LDC;

		link++;
		link_info++;
		current_np++;

		of_node_put(cpu);
		of_node_put(codec);
	}

	return 0;

err_put_codec:
	of_node_put(codec);
err_put_cpu:
	of_node_put(cpu);
err_put_np:
	of_node_put(np);

	return ret;
}
EXPORT_SYMBOL(cix_card_parse_of);

enum {
	LK_I2S0_SC_ALC5682_CODEC = 0,
	LK_I2S3_MC_ALC1019_PA,
	LK_I2S5_DP0,
	LK_I2S6_DP1,
	LK_I2S7_DP2,
	LK_I2S8_DP3,
	LK_I2S9_DP4,

	LK_MAX
};

/*
 * COMP_CPU with "dai_name" only, works well in DT binding, cause
 * component will be matched by "of_node".
 * But for acpi we need specify the "name" attribute to match component.
 */
SND_SOC_DAILINK_DEFS(cix_i2s0_sc_alc5682,
	DAILINK_COMP_ARRAY({.name = "CIXH6010:00", .dai_name = "i2s-sc-aif",}),
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-RTL5682:00", "rt5682s-aif1")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("CIXH6010:00")));
SND_SOC_DAILINK_DEFS(cix_i2s3_mc_alc1019,
	DAILINK_COMP_ARRAY({.name = "CIXH6011:00", .dai_name = "i2s-mc-aif1",}),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("CIXH6011:00")));
SND_SOC_DAILINK_DEFS(cix_i2s5_dp0,
	DAILINK_COMP_ARRAY({.name = NULL, .dai_name = "i2s-mc-aif1", }),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "i2s-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));
SND_SOC_DAILINK_DEFS(cix_i2s6_dp1,
	DAILINK_COMP_ARRAY({.name = NULL, .dai_name = "i2s-mc-aif1", }),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "i2s-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));
SND_SOC_DAILINK_DEFS(cix_i2s7_dp2,
	DAILINK_COMP_ARRAY({.name = NULL, .dai_name = "i2s-mc-aif1", }),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "i2s-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));
SND_SOC_DAILINK_DEFS(cix_i2s8_dp3,
	DAILINK_COMP_ARRAY({.name = NULL, .dai_name = "i2s-mc-aif1", }),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "i2s-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));
SND_SOC_DAILINK_DEFS(cix_i2s9_dp4,
	DAILINK_COMP_ARRAY({.name = NULL, .dai_name = "i2s-mc-aif1", }),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "i2s-hifi")),
	DAILINK_COMP_ARRAY(COMP_PLATFORM(NULL)));

static struct dai_link_info sky1_link_info[] = {
	{
		.jack_pin[JACK_HP].pin = "Headset",
		.jack_pin[JACK_HP].mask = SND_JACK_HEADSET,
		.jack_det_mask = JACK_MASK_HP,
		.mclk_fs = 512,
		.mclk_idx = 0,
	}, /* "dailink_i2s_sc0_alc5682" */
	{
	}, /* "dailink_i2s_mc3_alc1019" */
	{
	}, /* "dailink_i2s5_dp0 */
	{
	}, /* "dailink_i2s6_dp1 */
	{
	}, /* "dailink_i2s7_dp2 */
	{
	}, /* "dailink_i2s8_dp3 */
	{
	}, /* "dailink_i2s9_dp4 */
};

static struct snd_soc_dai_link sky1_dailink[] = {
	{
		.name = "dailink_i2s_sc0_alc5682",
		.stream_name = "soc:i2s-sc0",
		.id = 0, // i2s0
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s0_sc_alc5682),
	},
	{
		.name = "dailink_i2s_m2a_alc1019",
		.stream_name = "soc:i2s-m2a",
		.id = 3, // i2s3
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s3_mc_alc1019),
	},
	{
		.name = "dailink_i2s_m5a_dp0",
		.stream_name = "soc:i2s-m5a",
		.id = 5, // i2s5
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_IB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s5_dp0),
	},
	{
		.name = "dailink_i2s_m5b_dp1",
		.stream_name = "soc:i2s-m5b",
		.id = 6, // i2s6
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_IB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s6_dp1),
	},
	{
		.name = "dailink_i2s_m5c_dp2",
		.stream_name = "soc:i2s-m5c",
		.id = 7, // i2s7
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_IB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s7_dp2),
	},
	{
		.name = "dailink_i2s_m5d_dp3",
		.stream_name = "soc:i2s-m5d",
		.id = 8, // i2s8
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_IB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s8_dp3),
	},
	{
		.name = "dailink_i2s_m5e_dp4",
		.stream_name = "soc:i2s-m5e",
		.id = 9, // i2s9
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_IB_NF
				| SND_SOC_DAIFMT_GATED
				| SND_SOC_DAIFMT_CBC_CFC,
		.init = &cix_dailink_init,
		.ops = &cix_dailink_ops,
		SND_SOC_DAILINK_REG(cix_i2s9_dp4),
	},
};

static int cix_acpi_i2s_audio_check_present(int idx)
{
	struct acpi_device *adev;
	char path[16];
	acpi_handle handle;
	acpi_status status;

	snprintf(path, 16, "\\_SB.I2S%d", idx);
	status = acpi_get_handle(NULL, path, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev || !adev->status.present)
		return -ENODEV;

	return 0;
}

static int cix_acpi_dp_audio_check_present(int idx)
{
	struct acpi_device *adev;
	char path[16];
	acpi_handle handle;
	acpi_status status;

	snprintf(path, 16, "\\_SB.I2S%d", idx + SKY1_AUDSS_DP_AUDIO_OFFSET);
	status = acpi_get_handle(NULL, path, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev || !adev->status.present)
		return -ENODEV;

	snprintf(path, 16, "\\_SB.DP0%d", idx);
	status = acpi_get_handle(NULL, path, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev || !adev->status.present)
		return -ENODEV;

	return 0;
}

static struct device *cix_acpi_dp_audio_get_i2s_dev(int idx)
{
	struct acpi_device *adev;
	struct device *dev;
	char path[16];
	acpi_handle handle;
	acpi_status status;

	snprintf(path, 16, "\\_SB.I2S%d", idx + SKY1_AUDSS_DP_AUDIO_OFFSET);
	status = acpi_get_handle(NULL, path, &handle);
	if (ACPI_FAILURE(status))
		return ERR_PTR(-ENODEV);

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev || !adev->status.present)
		return ERR_PTR(-ENODEV);

	dev = bus_find_device_by_acpi_dev(&platform_bus_type, adev);
	if (!dev)
		return ERR_PTR(-EPROBE_DEFER);

	return dev;
}

/*
 * A DP device has several children: the hdmi-codec platform device, an i2c
 * adapter, and fw_devlink consumer:/supplier: link devices (plus a wakeup
 * source). device_find_any_child() returns an arbitrary one, so match the
 * hdmi-codec explicitly by its platform driver name.
 */
static int cix_dp_match_hdmi_codec(struct device *dev, const void *data)
{
	return dev->bus == &platform_bus_type &&
	       to_platform_device(dev)->name &&
	       !strcmp(to_platform_device(dev)->name, HDMI_CODEC_DRV_NAME);
}

static struct device *cix_acpi_dp_audio_get_codec_dev(int idx)
{
	struct acpi_device *adev;
	struct device *dev, *dpdev;
	char path[16];
	acpi_handle handle;
	acpi_status status;

	snprintf(path, 16, "\\_SB.DP0%d", idx);
	status = acpi_get_handle(NULL, path, &handle);
	if (ACPI_FAILURE(status))
		return NULL;

	adev = acpi_fetch_acpi_dev(handle);
	if (!adev || !adev->status.present)
		return NULL;

	dev = bus_find_device_by_acpi_dev(&platform_bus_type, adev);
	if (!dev)
		return ERR_PTR(-EPROBE_DEFER);

	dpdev = device_find_child(dev, NULL, cix_dp_match_hdmi_codec);
	put_device(dev);
	if (!dpdev)
		return ERR_PTR(-EPROBE_DEFER);

	return dpdev;
}

static int cix_card_link_supplier(struct device *consumer,
				  struct device *supplier)
{
	int ret;

	if (!device_link_add(consumer, supplier,
			     DL_FLAG_AUTOREMOVE_CONSUMER))
		return dev_err_probe(consumer, -EINVAL,
				     "failed to link supplier %s\n",
				     dev_name(supplier));

	device_lock(supplier);
	ret = device_is_bound(supplier) ? 0 : -EPROBE_DEFER;
	device_unlock(supplier);
	if (!ret)
		return 0;

	return dev_err_probe(consumer, ret, "supplier %s not ready\n",
			     dev_name(supplier));
}

int cix_card_parse_acpi(struct cix_asoc_card *priv)
{
	struct snd_soc_card *card = priv->card;
	struct snd_soc_dai_link *link;
	struct dai_link_info *link_info;
	struct device *dev = card->dev;
	struct snd_soc_codec_conf *codec_conf;
	int ret, i, j, num_links = 0, num_confs = 0;

	ret = cix_gpio_init(priv);
	if (ret)
		return ret;

	priv->cru_regmap = device_syscon_regmap_lookup_by_property(dev, "cru-ctrl");
	if (PTR_ERR(priv->cru_regmap) == -ENODEV) {
		priv->cru_regmap = NULL;
	} else if (IS_ERR(priv->cru_regmap)) {
		return PTR_ERR(priv->cru_regmap);
	}

	for (i = 0; i < (LK_MAX - SKY1_AUDSS_DP_AUDIO_COUNT); i++)
		if (!cix_acpi_i2s_audio_check_present(sky1_dailink[i].id))
			num_links++;
	for (i = 0; i < SKY1_AUDSS_DP_AUDIO_COUNT; i++)
		if (!cix_acpi_dp_audio_check_present(i))
			num_links++;
	if (num_links <= 0)
		return -ENODEV;
	dev_info(dev, "num_links = %d\n", num_links);

	link = devm_kcalloc(dev, num_links, sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	link_info = devm_kcalloc(dev, num_links, sizeof(*link_info), GFP_KERNEL);
	if (!link_info)
		return -ENOMEM;

	link->trigger_start = SND_SOC_TRIGGER_ORDER_DEFAULT;
	link->trigger_stop = SND_SOC_TRIGGER_ORDER_LDC;

	/*
	 * Each DP audio output binds an independent hdmi-audio-codec
	 * component. That codec sets no name_prefix, so with more than one
	 * DP present every instance contributes identically named DAPM
	 * widgets ("TX", "RX", "I2S Playback") to the same card, which makes
	 * the DAPM route resolver log "source/sink widget ... overwritten".
	 * Give each DP codec a unique prefix so its widgets stay distinct.
	 */
	codec_conf = devm_kcalloc(dev, SKY1_AUDSS_DP_AUDIO_COUNT,
				  sizeof(*codec_conf), GFP_KERNEL);
	if (!codec_conf)
		return -ENOMEM;

	card->name = "cix,sky1";
	card->num_links = num_links;
	card->dai_link = link;
	card->codec_conf = codec_conf;
	card->suspend_post = cix_card_suspend_post;
	card->resume_pre = cix_card_resume_pre;
	priv->link_info = link_info;

	/* on-board audio */
	for (i = 0; i < (LK_MAX - SKY1_AUDSS_DP_AUDIO_COUNT); i++) {
		if (cix_acpi_i2s_audio_check_present(sky1_dailink[i].id))
			continue;

		memcpy(link, &sky1_dailink[i], sizeof(struct snd_soc_dai_link));
		memcpy(link_info, &sky1_link_info[i], sizeof(struct dai_link_info));

		if (link_info->mclk_fs) {
			char mclk[16];

			snprintf(mclk, 16, "mclk%d", link_info->mclk_idx);
			dev_info(dev, "mclk = %s\n", mclk);

			link_info->clk_mclk = devm_clk_get(dev, mclk);
			if (IS_ERR(link_info->clk_mclk)) {
				dev_err(dev, "failed to get clk_mclk clock\n");
				return PTR_ERR(link_info->clk_mclk);
			}

			for (j = 0; j < AUDIO_CLK_NUM; j++) {
				link_info->clks[j] = devm_clk_get(dev, mclk_pll_names[j]);
				if (IS_ERR(link_info->clks[j])) {
					dev_err(dev, "failed to get clock %s\n", mclk_pll_names[j]);
					return PTR_ERR(link_info->clks[j]);
				}
			}
		}

		dev_info(dev, "audio: cpu[%s][%s] codec[%s][%s] platform[%s]\n",
			 link->cpus->name, link->cpus->dai_name,
			 link->codecs->name, link->codecs->dai_name,
			 link->platforms->name);
		link++;
		link_info++;
	}

	/* dp audio */
	for (i = 0; i < SKY1_AUDSS_DP_AUDIO_COUNT; i++) {
		struct device *codec_dev;
		struct device *i2s_dev;
		const char *codec_name;
		const char *cpu_name;
		char dp_str[32];

		if (cix_acpi_dp_audio_check_present(i))
			continue;

		i2s_dev = cix_acpi_dp_audio_get_i2s_dev(i);
		if (IS_ERR(i2s_dev)) {
			ret = PTR_ERR(i2s_dev);
			goto err;
		}

		codec_dev = cix_acpi_dp_audio_get_codec_dev(i);
		if (IS_ERR(codec_dev)) {
			ret = PTR_ERR(codec_dev);
			put_device(i2s_dev);
			goto err;
		}

		ret = cix_card_link_supplier(dev, i2s_dev);
		if (ret) {
			put_device(codec_dev);
			put_device(i2s_dev);
			goto err;
		}

		ret = cix_card_link_supplier(dev, codec_dev);
		if (ret) {
			put_device(codec_dev);
			put_device(i2s_dev);
			goto err;
		}

		cpu_name = devm_kstrdup(dev, dev_name(i2s_dev), GFP_KERNEL);
		codec_name = devm_kstrdup(dev, dev_name(codec_dev), GFP_KERNEL);
		put_device(codec_dev);
		put_device(i2s_dev);
		if (!cpu_name || !codec_name) {
			ret = -ENOMEM;
			goto err;
		}

		memcpy(link, &sky1_dailink[i + LK_I2S5_DP0], sizeof(struct snd_soc_dai_link));
		memcpy(link_info, &sky1_link_info[i + LK_I2S5_DP0], sizeof(struct dai_link_info));

		link->num_cpus = 1;
		link->num_codecs = 1;
		link->num_platforms = 1;

		link->cpus->name = cpu_name;
		link->codecs->name = codec_name;
		link->cpus->dai_name = "i2s-mc-aif1";
		link->codecs->dai_name = "i2s-hifi";
		link->platforms->name = link->cpus->name;

		snprintf(dp_str, sizeof(dp_str), "HDMI/DP,pcm=%d", (int)(link - card->dai_link));
		link_info->jack_pin[JACK_DPOUT].pin = devm_kstrdup(dev, dp_str, GFP_KERNEL);
		link_info->jack_pin[JACK_DPOUT].mask = SND_JACK_LINEOUT,
		link_info->jack_det_mask = JACK_MASK_DPOUT,

		dev_info(dev, "dp[%d]:cpu[%s][%s] codec[%s][%s] platform[%s]\n", i,
			 link->cpus->name, link->cpus->dai_name,
			 link->codecs->name, link->codecs->dai_name,
			 link->platforms->name);

		if (!link->cpus->name || !link->cpus->dai_name ||
		    !link->codecs->name || !link->codecs->dai_name ||
		    !link->platforms->name) {
			ret =  -EPROBE_DEFER;
			goto err;
		}

		codec_conf[num_confs].dlc.name = link->codecs->name;
		codec_conf[num_confs].name_prefix =
			devm_kasprintf(dev, GFP_KERNEL, "DP%d", i);
		if (!codec_conf[num_confs].name_prefix) {
			ret = -ENOMEM;
			goto err;
		}
		num_confs++;

		link++;
		link_info++;
	}

	card->num_configs = num_confs;

err:
	return ret;
}
EXPORT_SYMBOL(cix_card_parse_acpi);

MODULE_DESCRIPTION("Sound Card Utils for Cix Technology");
MODULE_AUTHOR("Xing.Wang <xing.wang@cixtech.com>");
MODULE_LICENSE("GPL v2");
