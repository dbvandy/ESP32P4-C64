/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 For the complete text of the GNU General Public License see
 http://www.gnu.org/licenses/.
*/
#ifndef I2SSOUND_H
#define I2SSOUND_H

#include "../Config.h"
#ifdef USE_I2SSOUND
#include "SoundDriver.h"
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>

class I2SSound : public SoundDriver {
private:
  i2s_chan_handle_t tx_channel = nullptr;
#if defined(BOARD_CROWPANEL_P4)
  // Elecrow amp expects stereo frames; SID produces mono.
  static constexpr size_t kStereoScratchSamples = 2048;
  int16_t stereoScratch[kStereoScratchSamples];
#endif

public:
  void init() override {
#if defined(BOARD_CROWPANEL_P4)
    // Match Elecrow BSP: I2S1, 16-bit Philips stereo.
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
#else
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
#endif
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_channel, NULL));

#if defined(BOARD_CROWPANEL_P4)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)Config::I2S_BCLK,
            .ws = (gpio_num_t)Config::I2S_LRC,
            .dout = (gpio_num_t)Config::I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags =
                {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
        }};
#else
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)Config::I2S_BCLK,
            .ws = (gpio_num_t)Config::I2S_LRC,
            .dout = (gpio_num_t)Config::I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags =
                {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
        }};
#endif

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_channel, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_channel));
  }

  void playAudio(int16_t *samples, size_t size) override {
    size_t bw = 0;
#if defined(BOARD_CROWPANEL_P4)
    const size_t n = size / sizeof(int16_t);
    size_t off = 0;
    while (off < n) {
      size_t chunk = n - off;
      if (chunk * 2 > kStereoScratchSamples) {
        chunk = kStereoScratchSamples / 2;
      }
      for (size_t i = 0; i < chunk; i++) {
        stereoScratch[i * 2] = samples[off + i];
        stereoScratch[i * 2 + 1] = samples[off + i];
      }
      ESP_ERROR_CHECK(i2s_channel_write(tx_channel, stereoScratch,
                                        chunk * 2 * sizeof(int16_t), &bw,
                                        portMAX_DELAY));
      off += chunk;
    }
#else
    ESP_ERROR_CHECK(
        i2s_channel_write(tx_channel, samples, size, &bw, portMAX_DELAY));
#endif
  }

  ~I2SSound() {
    if (tx_channel) {
      i2s_channel_disable(tx_channel);
      i2s_del_channel(tx_channel);
    }
  }
};
#endif

#endif // I2SSOUND_H
