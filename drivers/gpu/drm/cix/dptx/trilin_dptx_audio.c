// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Cix Technology Group Co., Ltd. */
#include <linux/kernel.h>
#include <linux/hdmi.h>
#include "trilin_dptx_reg.h"
#include "trilin_dptx.h"
#include "dptx_infoframe.h"

/* dp audio */
void dptx_audio_handle_plugged_change(struct dptx_audio *dp_audio, bool plugged)
{
	if (dp_audio->codec_dev && dp_audio->plugged_cb)
		dp_audio->plugged_cb(dp_audio->codec_dev, plugged);
}

static void dptx_audio_setup(struct trilin_dp *dp, int source, int freq,
			     int sample_len, int channel_count)
{
	unsigned int offset;
	unsigned int cs_length_orig_freq;
	unsigned int cs_freq_clock_accuracy;

	if (!dp)
		return;

	offset = (TRILIN_DPTX_SEC1_AUDIO_ENABLE -
		  TRILIN_DPTX_SEC0_AUDIO_ENABLE) *
		 source;

	/* Bit 7:4 source number; Bit 3, '0' for linear PCM samples; */
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_CS_SOURCE_FORMAT + offset,
			(source << DPTX_CS_SOURCE_NUMBER_SHIFT));
	/* Categroy code: 0, General. */
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_CS_CATEGORY_CODE + offset,
			DPTX_CS_CATEGORY_CODE);
	switch (sample_len) {
	case 16:
		cs_length_orig_freq = DPTX_CS_SAMPLE_WORD_LENGTH_16BITS;
		break;
	case 18:
		cs_length_orig_freq = DPTX_CS_SAMPLE_WORD_LENGTH_18BITS;
		break;
	case 20:
		cs_length_orig_freq = DPTX_CS_SAMPLE_WORD_LENGTH_20BITS;
		break;
	case 24:
	default:
		cs_length_orig_freq = DPTX_CS_SAMPLE_WORD_LENGTH_24BITS;
		break;
	}
	/* clock accuracy: 00, level II, default; 10, level I; 01, level III */
	switch (freq) {
	case 32000:
		cs_length_orig_freq |= DPTX_CS_SAMPLING_ORIG_FREQ_32000HZ;
		cs_freq_clock_accuracy = DPTX_CS_SAMPLING_FREQ_32000Hz;
		break;
	case 44100:
		cs_length_orig_freq |= DPTX_CS_SAMPLING_ORIG_FREQ_44100HZ;
		cs_freq_clock_accuracy = DPTX_CS_SAMPLING_FREQ_44100Hz;
		break;
	case 48000:
	default:
		cs_length_orig_freq |= DPTX_CS_SAMPLING_ORIG_FREQ_48000HZ;
		cs_freq_clock_accuracy = DPTX_CS_SAMPLING_FREQ_48000Hz;
		break;
	}

	trilin_dp_write(dp, TRILIN_DPTX_SEC0_CS_LENGTH_ORIG_FREQ + offset,
			cs_length_orig_freq);
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_CS_FREQ_CLOCK_ACCURACY + offset,
			cs_freq_clock_accuracy);
	/* copyright*/
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_CS_COPYRIGHT + offset,
			DPTX_CS_COPYRIGHT);
	/* Every other register here is offset by source; this one was not. */
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_TIMESTAMP_INTERVAL + offset,
			DPTX_CS_TIMESTAMP_INTERVAL);
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_AUDIO_CHANNEL_MAP + offset,
			DPTX_CS_AUDIO_CHANNEL_MAP_DEFAULT);
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_CHANNEL_COUNT + offset,
			channel_count);
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_INPUT_SELECT + offset, source);
}

static void dptx_audio_send_infoframe(struct trilin_dp *dp, int source,
				      const struct hdmi_audio_infoframe *cea)
{
	struct dp_sdp sdp;
	ssize_t len;

	len = hdmi_audio_infoframe_pack_for_dp(cea, &sdp, dp->dpcd[DP_DPCD_REV]);
	if (len < 0) {
		dev_warn(dp->dev, "failed to pack audio infoframe: %zd\n", len);
		return;
	}

	cix_infoframe_write_packet(dp, source, CIX_AUDIO_INFO_SDP, &sdp, 1);
}

static int dptx_audio_get_eld(struct device *dev, void *data,
			      uint8_t *buf, size_t len)
{
	struct trilin_dp *dp = data;
	struct drm_connector *connector = &dp->connector.base;

	mutex_lock(&connector->eld_mutex);
	if (trilin_dp_plugged_status(dp))
		memcpy(buf, connector->eld, min(sizeof(connector->eld), len));
	else
		memset(buf, 0, len);
	mutex_unlock(&connector->eld_mutex);

	return 0;
}

static int dptx_audio_startup(struct device *dev, void *data)
{
	struct trilin_dp *dp = (struct trilin_dp *)data;
	struct dptx_audio *dp_audio = &dp->dp_audio;

	/* No sink: do not enable playback or parse an intentionally empty ELD. */
	if (!trilin_dp_plugged_status(dp))
		return -ENODEV;

	trilin_dp_write(dp, TRILIN_DPTX_SEC0_AUDIO_ENABLE, 1);

	dp_audio->running = true;

	return 0;
}

static void dptx_audio_shutdown(struct device *dev, void *data)
{
	struct trilin_dp *dp = (struct trilin_dp *)data;
	struct dptx_audio *dp_audio = &dp->dp_audio;

	trilin_dp_write(dp, TRILIN_DPTX_SEC0_AUDIO_ENABLE, 0);
	dp_audio->running = false;
}

static int dptx_audio_hw_params(struct device *dev, void *data,
				struct hdmi_codec_daifmt *daifmt,
				struct hdmi_codec_params *params)
{
	struct trilin_dp *dp = (struct trilin_dp *)data;
	struct dptx_audio *dp_audio = &dp->dp_audio;

	if (!trilin_dp_plugged_status(dp))
		return 0;

	dp_audio->params.sample_width = params->sample_width;
	dp_audio->params.sample_rate = params->sample_rate;
	dp_audio->params.channels = params->channels;
	dp_audio->params.cea = params->cea;

	dev_dbg(dev,
		"%s, daifmt fmt:%d, bit_clk_inv:%d, frame_clk_inv:%d, bit_clk_master:%d, frame_clk_master:%d, params sample_rate:%d, sample_width:%d, channels:%d, ca:%d\n",
		__func__, daifmt->fmt, daifmt->bit_clk_inv,
		daifmt->frame_clk_inv, daifmt->bit_clk_provider,
		daifmt->frame_clk_provider, params->sample_rate,
		params->sample_width, params->channels,
		params->cea.channel_allocation);

	/*
	 * hdmi-codec calls audio_startup() BEFORE hw_params(), so by the time we
	 * get here SEC0_AUDIO_ENABLE is already 1 and the sink is receiving an
	 * audio stream. dptx_audio_setup() then rewrites the channel status
	 * (sampling frequency, word length, channel count) underneath it, so the
	 * sink briefly sees a live stream whose channel status does not describe
	 * it. That is audible as a glitch or a burst of noise every time the PCM
	 * is (re)configured -- which is at the start of every playback, because
	 * the HDMI sink node is created with node.suspend-on-idle=true and is
	 * torn down after a few seconds of silence. A YouTube advert/content
	 * boundary is exactly that: silence, suspend, resume, reconfigure.
	 *
	 * Mute the SDP audio stream across the reprogramming and restore it
	 * afterwards, so the sink only ever sees channel status that matches the
	 * samples being sent.
	 */
	if (dp_audio->running)
		trilin_dp_write(dp, TRILIN_DPTX_SEC0_AUDIO_ENABLE, 0);

	dptx_audio_setup(dp, 0, params->sample_rate, params->sample_width,
			 params->channels);
	dptx_audio_send_infoframe(dp, 0, &params->cea);

	if (dp_audio->running)
		trilin_dp_write(dp, TRILIN_DPTX_SEC0_AUDIO_ENABLE, 1);

	return 0;
}

static int dptx_audio_get_dai_id(struct snd_soc_component *comment,
				 struct device_node *endpoint, void *data)
{
	return 0;
}

static int dptx_audio_hook_plugged_cb(struct device *dev, void *data,
				      hdmi_codec_plugged_cb fn,
				      struct device *codec_dev)
{
	struct trilin_dp *dp = (struct trilin_dp *)data;
	struct dptx_audio *dp_audio = &dp->dp_audio;
	bool connected;

	dp_audio->plugged_cb = fn;
	dp_audio->codec_dev = codec_dev;
	connected = trilin_dp_plugged_status(dp);

	/* dp plugin event report before this callback install when boot, have a check here */
	dev_dbg(dp->dev, "dp audio plugin status = %d\n", connected);
	dptx_audio_handle_plugged_change(dp_audio, connected);

	return 0;
}

void dptx_audio_reconfig_and_enable(void *data)
{
	struct trilin_dp *dp = (struct trilin_dp *)data;
	struct dptx_audio *dp_audio = &dp->dp_audio;

	dptx_audio_setup(dp, 0, dp_audio->params.sample_rate,
			 dp_audio->params.sample_width,
			 dp_audio->params.channels);
	dptx_audio_send_infoframe(dp, 0, &dp_audio->params.cea);

	/* enable dptx audio */
	trilin_dp_write(dp, TRILIN_DPTX_SEC0_AUDIO_ENABLE, 1);
}

const struct hdmi_codec_ops dptx_audio_codec_ops = {
	.hw_params = dptx_audio_hw_params,
	.audio_startup = dptx_audio_startup,
	.audio_shutdown = dptx_audio_shutdown,
	.get_eld = dptx_audio_get_eld,
	.get_dai_id = dptx_audio_get_dai_id,
	.hook_plugged_cb = dptx_audio_hook_plugged_cb
};
