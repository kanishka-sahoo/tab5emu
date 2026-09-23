/*
 * Tab5 audio: ES8388 DAC over I2S, through esp_codec_dev.
 *
 * The I2S channels are created here rather than by bsp_audio_init(), which
 * hard-codes the DMA queue (6 x 240 frames = 30 ms at 48 kHz; bring-up).
 * The audio engine picks its own depth for latency (plan D3, spec §15).
 * Both directions are created, as the BSP does, so the ES7210 microphone
 * path can share the bus later (spec §16).
 */
#include "bsp/m5stack_tab5.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "retro_log.h"
#include "retro_tab5.h"

static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_spk;
static unsigned s_rate, s_desc, s_frames;

static bool create_i2s(unsigned rate, unsigned desc, unsigned frames)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    cc.dma_desc_num = desc;
    cc.dma_frame_num = frames;
    cc.auto_clear = true; /* an underrun plays silence, not the last buffer */
    if (i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) {
        return false;
    }
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    return i2s_channel_init_std_mode(s_tx, &std) == ESP_OK && i2s_channel_enable(s_tx) == ESP_OK &&
           i2s_channel_init_std_mode(s_rx, &std) == ESP_OK && i2s_channel_enable(s_rx) == ESP_OK;
}

/* What bsp_audio_codec_speaker_init() does, on our I2S channels. */
static esp_codec_dev_handle_t create_speaker(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) {
        return NULL;
    }
    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO, /* none: the amp is on the IO expander */
        .pa_reverted = false,
        .master_mode = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = es8388_codec_new(&codec_cfg);
    if (!codec) {
        return NULL;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec,
        .data_if = data_if,
    };
    return esp_codec_dev_new(&dev_cfg);
}

bool retro_tab5_audio_open(unsigned sample_rate, unsigned dma_desc, unsigned dma_frames)
{
    if (s_spk) {
        /* The channel can't be reconfigured once the codec shares it; keep
         * the first geometry (retro_tab5_audio_geometry() reports it). */
        if (sample_rate != s_rate) {
            RLOGE(AUDIO, "already open at %u Hz; %u Hz refused", s_rate, sample_rate);
            return false;
        }
        if (dma_desc != s_desc || dma_frames != s_frames) {
            RLOGW(AUDIO, "keeping DMA %u x %u (asked for %u x %u)", s_desc, s_frames, dma_desc,
                  dma_frames);
        }
        retro_tab5_audio_mute(false);
        return true;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }
    if (!create_i2s(sample_rate, dma_desc, dma_frames)) {
        RLOGE(AUDIO, "I2S init failed");
        return false;
    }
    esp_codec_dev_handle_t spk = create_speaker();
    if (!spk) {
        RLOGE(AUDIO, "ES8388 init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
        RLOGE(AUDIO, "codec open failed");
        return false;
    }
    retro_tab5_rail_set(RETRO_TAB5_RAIL_SPEAKER, true);
    s_spk = spk;
    s_rate = sample_rate;
    s_desc = dma_desc;
    s_frames = dma_frames;
    RLOGI(AUDIO, "ES8388 open: %u Hz stereo s16, DMA %u x %u frames", sample_rate, dma_desc,
          dma_frames);
    return true;
}

bool retro_tab5_audio_init(unsigned sample_rate)
{
    /* The BSP's default queue, as measured in bring-up. */
    return retro_tab5_audio_open(sample_rate, 6, 240);
}

bool retro_tab5_audio_write(const int16_t *stereo, size_t frames)
{
    return retro_tab5_audio_write_timeout(stereo, frames, UINT32_MAX) == frames;
}

size_t retro_tab5_audio_write_timeout(const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    if (!s_tx) {
        return 0;
    }
    size_t written = 0;
    const TickType_t to = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    i2s_channel_write(s_tx, stereo, frames * 4, &written, to);
    return written / 4;
}

bool retro_tab5_audio_volume(int percent)
{
    return s_spk && esp_codec_dev_set_out_vol(s_spk, percent) == ESP_CODEC_DEV_OK;
}

bool retro_tab5_audio_mute(bool mute)
{
    return s_spk && esp_codec_dev_set_out_mute(s_spk, mute) == ESP_CODEC_DEV_OK;
}

void retro_tab5_audio_geometry(unsigned *dma_desc, unsigned *dma_frames)
{
    *dma_desc = s_desc;
    *dma_frames = s_frames;
}

size_t retro_tab5_audio_dma_frames(void)
{
    return (size_t)s_desc * s_frames;
}
