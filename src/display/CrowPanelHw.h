/*
 Shared CrowPanel panel / framebuffer / touch access for display + touch KB.

 Landscape FB: C64 on the LEFT, keyboard on the RIGHT.
 Both drawn 90° CCW so that when held upright, C64 is on top and keyboard below.
 C64 viewport is 4:3 when upright. Keyboard unit cells are square.
*/
#ifndef CROWPANEL_HW_H
#define CROWPANEL_HW_H

#include "../Config.h"
#ifdef USE_CROWPANEL_DISPLAY

#include <cstdint>
namespace esp_panel {
namespace board {
class Board;
}
namespace drivers {
class LCD;
class Touch;
} // namespace drivers
} // namespace esp_panel

namespace CrowPanelHw {
constexpr uint16_t PANEL_W = 1024;
constexpr uint16_t PANEL_H = 600;
constexpr uint16_t SRC_W = 320;
constexpr uint16_t SRC_H = 200;
// Source after 90° CCW (for sampling)
constexpr uint16_t ROT_W = SRC_H; // 200
constexpr uint16_t ROT_H = SRC_W; // 320

// Upright 4:3 → landscape rect is 3:4 (W:H). Fill full panel height.
// upright W = DST_H, upright H = DST_W, W/H = 4/3 → DST_H/DST_W = 4/3
constexpr uint16_t DST_H = PANEL_H;              // 600 (= upright width)
constexpr uint16_t DST_W = (DST_H * 3) / 4;      // 450 (= upright height)
constexpr uint16_t ORIGIN_X = 0;
constexpr uint16_t ORIGIN_Y = 0;

// Keyboard region = remaining width, full height
constexpr uint16_t KB_X0 = DST_W;                // 450
constexpr uint16_t KB_W = PANEL_W - KB_X0;       // 574
constexpr uint16_t KB_H = PANEL_H;               // 600

// Portrait-logical keyboard before 90° CCW into KB region
constexpr uint16_t KB_PORTRAIT_W = PANEL_H;      // 600
constexpr uint16_t KB_PORTRAIT_H = KB_W;         // 574

esp_panel::board::Board *board();
esp_panel::drivers::LCD *lcd();
esp_panel::drivers::Touch *touch();
uint16_t *framebuffer();
void present();

void requestKeyboardRedraw();
bool keyboardNeedsRedraw();
void clearKeyboardRedraw();

void setStickyMods(uint8_t m);
uint8_t stickyMods();
void setHighlight(int idx);
int highlight();

// Attached .d64 basename shown next to SPACE (empty clears / eject).
void setAttachedDisk(const char *name);
const char *attachedDisk();

// 1541 activity LED (red dot next to disk name while sectors are read).
void pulseDriveActivity();
void tickDriveActivity();
bool driveReading();
} // namespace CrowPanelHw

#endif
#endif
