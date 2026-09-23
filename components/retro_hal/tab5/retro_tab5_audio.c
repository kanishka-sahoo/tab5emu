/*
 * Tab5 audio: ES8388 DAC over I2S through the BSP and esp_codec_dev.
 *
 * Phase 1 only needs blocking playback for the bring-up tests. Phase 2's
 * audio_engine replaces this with its own I2S feeder task (plan D3).
 */
#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"

#include "retro_log.h"
#include "retro_tab5.h"

/* bsp_audio_init() creates the channel with I2S_CHANNEL_DEFAULT_CONFIG:
 * 6 DMA descriptors x 240 frames. */
#define BSP_I2S_DMA_DESC 6
#define BSP_I2S_DMA_FRAMES 240

static esp_codec_dev_handle_t s_spk;
static unsigned s_rate;

bool retro_tab5_audio_init(unsigned sample_rate)
{
    if (s_spk) {
        return sample_rate == s_rate;
    }
    if (!retro_tab5_board_init()) {
        return false;
    }

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        RLOGE(AUDIO, "I2S init failed: %s", esp_err_to_name(err));
        return false;
    }
    esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
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
    s_spk = spk;
    s_rate = sample_rate;
    RLOGI(AUDIO, "ES8388 open: %u Hz stereo s16, DMA %u x %u frames", sample_rate,
          BSP_I2S_DMA_DESC, BSP_I2S_DMA_FRAMES);
    return true;
}

bool retro_tab5_audio_write(const int16_t *stereo, size_t frames)
{
    if (!s_spk) {
        return false;
    }
    return esp_codec_dev_write(s_spk, (void *)stereo, (int)(frames * 4)) == ESP_CODEC_DEV_OK;
}

bool retro_tab5_audio_volume(int percent)
{
    return s_spk && esp_codec_dev_set_out_vol(s_spk, percent) == ESP_CODEC_DEV_OK;
}

size_t retro_tab5_audio_dma_frames(void)
{
    return BSP_I2S_DMA_DESC * BSP_I2S_DMA_FRAMES;
}
