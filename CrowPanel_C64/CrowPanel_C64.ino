/*
 CrowPanel Advanced 10.1" ESP32-P4 port of T-HMI-C64.
 Flash to COM5 (UART0 / CH340).
*/

#undef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 5

#include "C64Emu.h"
#include "platform/PlatformFactory.h"
#include "platform/PlatformManager.h"

static const char *TAG = "CrowPanel-C64";

TaskHandle_t core0TaskHandle;

C64Emu c64Emu;

void core0Task(void *param) {
  PlatformManager::initialize(PlatformNS::create());
  try {
    c64Emu.setup();
  } catch (...) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "setup() failed");
    while (true) {
      PlatformManager::getInstance().waitMS(500);
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
  PlatformManager::getInstance().log(
      LOG_INFO, TAG, "setup() running on core %d", xPortGetCoreID());
  PlatformManager::getInstance().log(LOG_INFO, TAG, "setup done");

  while (true) {
    c64Emu.loop();
  }
}

void setup() {
  Serial.begin(115200);
  vTaskDelay(pdMS_TO_TICKS(500));
  xTaskCreatePinnedToCore(core0Task, "core0Task", 12288, nullptr, 1,
                          &core0TaskHandle, 0);
}

void loop() {}
