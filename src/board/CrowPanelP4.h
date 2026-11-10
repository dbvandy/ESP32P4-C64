/*
 CrowPanel Advanced 10.1" (ESP32-P4) board driver for T-HMI-C64.
*/
#ifndef CROWPANEL_P4_H
#define CROWPANEL_P4_H

#include "../Config.h"
#ifdef BOARD_CROWPANEL_P4
#include "BoardDriver.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_ldo_regulator.h>

class CrowPanelP4 : public BoardDriver {
  static bool enableLdo(int chan, int mv) {
    esp_ldo_channel_handle_t handle = nullptr;
    esp_ldo_channel_config_t cfg = {
        .chan_id = chan,
        .voltage_mv = mv,
    };
    return esp_ldo_acquire_channel(&cfg, &handle) == ESP_OK;
  }

public:
  void init() override {
    // MIPI PHY / panel rails used by Elecrow CrowPanel examples
    enableLdo(3, 2500);
    // LDO4 also feeds SDMMC IO (GPIO39-44). Hold it here so SD_MMC must
    // not re-acquire channel 4 (that fails with "already in use").
    enableLdo(4, 3300);

    pinMode(Config::LCD_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(Config::LCD_BACKLIGHT_PIN, Config::LCD_BACKLIGHT_ON_LEVEL);

    // Speaker amplifier enable (NS4168 CTRL is active-low on CrowPanel)
    pinMode(Config::SPK_AMP_PIN, OUTPUT);
    digitalWrite(Config::SPK_AMP_PIN, LOW);

    // Optional backlight power rail
    pinMode(Config::LCD_BK_POWER_PIN, OUTPUT);
    digitalWrite(Config::LCD_BK_POWER_PIN, HIGH);

    // microSD card power (active high). Do this before Floppy::init.
    pinMode(Config::SD_POWER_PIN, OUTPUT);
    digitalWrite(Config::SD_POWER_PIN, HIGH);
  }

  uint16_t getBatteryVoltage() override { return 4000; }

  void powerOff() override {}
};

#endif
#endif // CROWPANEL_P4_H
