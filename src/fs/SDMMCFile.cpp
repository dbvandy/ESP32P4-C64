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
#include "../Config.h"
#ifdef USE_SDCARD
#include "../platform/PlatformManager.h"
#include "SDMMCFile.h"

static const char *TAG = "SDMMCFile";

bool SDMMCFile::initialized = false;

#if defined(BOARD_CROWPANEL_P4)
#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static sdmmc_card_t *s_card = nullptr;
static DIR *s_listdir = nullptr;
static sdmmc_host_t s_host{};
static sdmmc_slot_config_t s_slot{};
static bool s_hostReady = false;

static bool mountCard() {
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 8,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = false,
  };
  esp_err_t ret =
      esp_vfs_fat_sdmmc_mount("/sdcard", &s_host, &s_slot, &mount_config, &s_card);
  return ret == ESP_OK;
}

bool SDMMCFile::init() {
  if (SDMMCFile::initialized) {
    return true;
  }
  // Card power + settle (board driver also raises SD_POWER_PIN).
  pinMode(Config::SD_POWER_PIN, OUTPUT);
  digitalWrite(Config::SD_POWER_PIN, HIGH);
  PlatformManager::getInstance().waitMS(250);

  if (!s_hostReady) {
    // CrowPanel: 1-bit SDMMC on CLK/CMD/D0. LDO4 already held by CrowPanelP4 —
    // do NOT call sd_pwr_ctrl_new_on_chip_ldo (Arduino SD_MMC.begin would).
    // Also avoid BOARD_SDMMC_POWER_PIN=45, which is touch I2C SDA on this board.
    s_host = SDMMC_HOST_DEFAULT();
    s_host.flags = SDMMC_HOST_FLAG_1BIT;
    s_host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    s_host.slot = SDMMC_HOST_SLOT_0;

    s_slot = SDMMC_SLOT_CONFIG_DEFAULT();
    s_slot.clk = (gpio_num_t)Config::SD_SCLK_PIN;
    s_slot.cmd = (gpio_num_t)Config::SD_MOSI_PIN;
    s_slot.d0 = (gpio_num_t)Config::SD_MISO_PIN;
    s_slot.d1 = GPIO_NUM_NC;
    s_slot.d2 = GPIO_NUM_NC;
    s_slot.d3 = GPIO_NUM_NC;
    s_slot.width = 1;
    s_slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    s_hostReady = true;
  }

  if (!mountCard()) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "sd mount failed");
    return false;
  }
  SDMMCFile::initialized = true;
  PlatformManager::getInstance().log(LOG_INFO, TAG,
                                     "CrowPanel SD mounted (1541 host FS)");
  return true;
}

bool SDMMCFile::open(const std::string &path, const char *mode) {
  close();
  std::string full = "/sdcard/" + path;
  fp = fopen(full.c_str(), mode);
  return fp != nullptr;
}

size_t SDMMCFile::read(void *buffer, size_t count) {
  return fp ? fread(buffer, 1, count, fp) : 0;
}

size_t SDMMCFile::write(const void *buffer, size_t count) {
  if (!fp) {
    return 0;
  }
  size_t n = fwrite(buffer, 1, count, fp);
  // Push through FatFS so D64 sector writes survive a power-cycle / remount.
  fflush(fp);
#if defined(BOARD_CROWPANEL_P4)
  fsync(fileno(fp));
#endif
  return n;
}

bool SDMMCFile::seek(long offset, int origin) {
  return fp ? (fseek(fp, offset, origin) == 0) : false;
}

long SDMMCFile::tell() const { return fp ? ftell(fp) : -1; }

bool SDMMCFile::eof() { return fp ? (feof(fp) != 0) : true; }

int64_t SDMMCFile::size() {
  if (!fp) {
    return -1;
  }
  long cur = ftell(fp);
  if (cur < 0) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    return -1;
  }
  long end = ftell(fp);
  fseek(fp, cur, SEEK_SET);
  return end;
}

void SDMMCFile::close() {
  if (fp) {
    fclose(fp);
    fp = nullptr;
  }
}

bool SDMMCFile::listnextentry(std::string &name, bool start) {
  name = "";
  if (!SDMMCFile::initialized) {
    return false;
  }
  if (start) {
    if (s_listdir) {
      closedir(s_listdir);
      s_listdir = nullptr;
    }
    s_listdir = opendir("/sdcard");
    if (!s_listdir) {
      PlatformManager::getInstance().log(LOG_INFO, TAG, "cannot open root dir");
      return false;
    }
  }
  if (!s_listdir) {
    return false;
  }
  struct dirent *ent;
  while ((ent = readdir(s_listdir)) != nullptr) {
    if (ent->d_name[0] == '.') {
      continue;
    }
    name = ent->d_name;
    return true;
  }
  closedir(s_listdir);
  s_listdir = nullptr;
  return true;
}

SDMMCFile::~SDMMCFile() { close(); }

#else
#include <SD_MMC.h>

bool SDMMCFile::init() {
  if (SDMMCFile::initialized) {
    return true;
  }
  PlatformManager::getInstance().waitMS(500);
  SD_MMC.setPins(Config::SD_SCLK_PIN, Config::SD_MOSI_PIN, Config::SD_MISO_PIN);
  bool rlst = SD_MMC.begin("/sdcard", true);
  if (!rlst) {
    return false;
  }
  SDMMCFile::initialized = true;
  return true;
}

bool SDMMCFile::open(const std::string &path, const char *mode) {
  close();
  const char *m = (mode[0] == 'r') ? FILE_READ : FILE_WRITE;
  std::string path1 = '/' + path;
  file = SD_MMC.open(path1.c_str(), m);
  return file;
}

size_t SDMMCFile::read(void *buffer, size_t count) {
  return file ? file.read((uint8_t *)buffer, count) : 0;
}

size_t SDMMCFile::write(const void *buffer, size_t count) {
  return file ? file.write((const uint8_t *)buffer, count) : 0;
}

bool SDMMCFile::seek(long offset, int origin) {
  if (!file) {
    return false;
  }
  if (origin == SEEK_SET) {
    return file.seek(offset);
  }
  if (origin == SEEK_CUR) {
    return file.seek(file.position() + offset);
  }
  if (origin == SEEK_END) {
    return file.seek(file.size() + offset);
  }
  return false;
}

long SDMMCFile::tell() const { return file ? file.position() : -1; }

bool SDMMCFile::eof() { return file && (!file.available()); }

int64_t SDMMCFile::size() {
  return file ? static_cast<int64_t>(file.size()) : -1;
}

void SDMMCFile::close() {
  if (file) {
    file.close();
  }
}

File listroot;

bool SDMMCFile::listnextentry(std::string &name, bool start) {
  name = "";
  if (!SDMMCFile::initialized) {
    return false;
  }
  File file;
  if (start) {
    if (listroot) {
      listroot.close();
    }
    listroot = SD_MMC.open("/");
    if (!listroot || !listroot.isDirectory()) {
      PlatformManager::getInstance().log(LOG_INFO, TAG, "cannot open root dir");
      return false;
    }
    file = listroot.openNextFile();
  } else {
    file = listroot.openNextFile();
  }
  if (!file) {
    listroot.close();
    return true;
  }
  name = file.name();
  return true;
}

SDMMCFile::~SDMMCFile() { close(); }
#endif
#endif
