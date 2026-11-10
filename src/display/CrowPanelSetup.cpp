/*
 CrowPanel SETUP: brightness / volume sliders + NVS.
 Laid out in portrait space (same frame as the keyboard keys) so the
 sheet appears 90° CCW on the landscape KB strip — matching key orientation.
*/
#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include "CrowPanelSetup.h"
#include "CrowPanelHw.h"
#include "CrowPanelKeyLayout.h"
#include "../ExtCmd.h"
#include "../ExtCmdQueue.h"
#include "../platform/PlatformManager.h"
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <cstring>

namespace CrowPanelSetup {
namespace {

static const char *TAG = "Setup";
static bool s_active = false;
static uint8_t s_bright = 80; // 0..100
static uint8_t s_volume = Config::DEFAULT_VOLUME;
static bool s_dragBright = false;
static bool s_dragVol = false;

struct Rect {
  int x, y, w, h;
};
static Rect s_panel{};
static Rect s_brightTrack{};
static Rect s_volTrack{};
static Rect s_brightHit{};
static Rect s_volHit{};
static Rect s_close{};

static bool inRect(const Rect &r, int px, int py) {
  return r.w > 0 && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static void portraitFromTouch(int tx, int ty, int &px, int &py) {
  px = (int)CrowPanelHw::PANEL_H - 1 - ty;
  py = tx - (int)CrowPanelHw::KB_X0;
}

static void layout() {
  // Portrait-native iPhone sheet (title on top, horizontal sliders).
  // crowKeyRect maps portrait (w,h) → screen (h,w), so this becomes a
  // tall card on the landscape strip — 90° CCW from a landscape card.
  const int pw = CrowPanelHw::KB_PORTRAIT_W;
  const int ph = CrowPanelHw::KB_PORTRAIT_H;
  const int cardW = 320; // portrait width  → on-screen height
  const int cardH = 220; // portrait height → on-screen width
  s_panel = {(pw - cardW) / 2, (ph - cardH) / 2, cardW, cardH};

  const int pad = 18;
  const int trackThick = 6;
  const int hitThick = 36;
  const int trackLen = s_panel.w - 2 * pad;
  const int titleBand = 36;
  const int rowPitch = 64;
  const int labelGap = 18;

  // Center the two slider rows in the content area below the title.
  const int contentTop = s_panel.y + titleBand;
  const int contentH = s_panel.h - titleBand - pad;
  const int blockH = labelGap + rowPitch + trackThick;
  const int blockTop = contentTop + (contentH - blockH) / 2;
  const int brightY = blockTop + labelGap;
  const int volY = brightY + rowPitch;

  s_brightTrack = {s_panel.x + pad, brightY - trackThick / 2, trackLen,
                   trackThick};
  s_volTrack = {s_panel.x + pad, volY - trackThick / 2, trackLen, trackThick};

  s_brightHit = {s_panel.x + pad - 4, brightY - hitThick / 2, trackLen + 8,
                 hitThick};
  s_volHit = {s_panel.x + pad - 4, volY - hitThick / 2, trackLen + 8, hitThick};

  // Close: top-trailing in portrait (matches keyboard "top").
  const int cr = 14;
  s_close = {s_panel.x + s_panel.w - 34, s_panel.y + 8, cr * 2, cr * 2};
}

static void applyBrightness() {
  static bool attached = false;
  if (!attached) {
    ledcAttach(Config::LCD_BACKLIGHT_PIN, 20000, 10);
    attached = true;
  }
  uint32_t duty = ((uint32_t)s_bright * 1023) / 100;
  if (duty < 40 && s_bright > 0) {
    duty = 40;
  }
  if (s_bright == 0) {
    duty = 0;
  }
  ledcWrite(Config::LCD_BACKLIGHT_PIN, duty);
}

static void applyVolume() {
  ExtCmdQueue::ExternalCmd ext{};
  ext.cmd = ExtCmd::SETVOLUME;
  memset(ext.param, 0, sizeof(ext.param));
  ext.param[0] = s_volume;
  ExtCmdQueue::getInstance().push(ext);
}

static void saveNvs() {
  Preferences prefs;
  if (!prefs.begin("csetup", false)) {
    return;
  }
  prefs.putUChar("bright", s_bright);
  prefs.putUChar("vol", s_volume);
  prefs.end();
}

static void loadNvs() {
  Preferences prefs;
  if (!prefs.begin("csetup", true)) {
    return;
  }
  s_bright = prefs.getUChar("bright", 80);
  s_volume = prefs.getUChar("vol", Config::DEFAULT_VOLUME);
  prefs.end();
  if (s_bright > 100) {
    s_bright = 100;
  }
}

// Value follows portrait X (horizontal in the key/portrait frame).
static void setBrightFromX(int px) {
  int t0 = s_brightTrack.x;
  int tw = s_brightTrack.w;
  if (tw <= 0) {
    return;
  }
  int v = ((px - t0) * 100) / tw;
  if (v < 0) {
    v = 0;
  }
  if (v > 100) {
    v = 100;
  }
  if ((uint8_t)v != s_bright) {
    s_bright = (uint8_t)v;
    applyBrightness();
    CrowPanelHw::requestKeyboardRedraw();
  }
}

static void setVolFromX(int px) {
  int t0 = s_volTrack.x;
  int tw = s_volTrack.w;
  if (tw <= 0) {
    return;
  }
  int v = ((px - t0) * 255) / tw;
  if (v < 0) {
    v = 0;
  }
  if (v > 255) {
    v = 255;
  }
  if ((uint8_t)v != s_volume) {
    s_volume = (uint8_t)v;
    applyVolume();
    CrowPanelHw::requestKeyboardRedraw();
  }
}

} // namespace

void init() {
  layout();
  loadNvs();
  applyBrightness();
  applyVolume();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "bright=%u vol=%u",
                                     (unsigned)s_bright, (unsigned)s_volume);
}

bool active() { return s_active; }

void open() {
  layout();
  s_active = true;
  s_dragBright = false;
  s_dragVol = false;
  CrowPanelHw::requestKeyboardRedraw();
}

void close() {
  if (!s_active) {
    return;
  }
  s_active = false;
  s_dragBright = false;
  s_dragVol = false;
  saveNvs();
  CrowPanelHw::requestKeyboardRedraw();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "saved bright=%u vol=%u",
                                     (unsigned)s_bright, (unsigned)s_volume);
}

void onTouch(int touchX, int touchY) {
  if (!s_active || touchX < (int)CrowPanelHw::KB_X0) {
    return;
  }
  int px, py;
  portraitFromTouch(touchX, touchY, px, py);
  if (s_dragBright) {
    setBrightFromX(px);
    return;
  }
  if (s_dragVol) {
    setVolFromX(px);
    return;
  }
  if (inRect(s_close, px, py)) {
    close();
    return;
  }
  if (inRect(s_brightHit, px, py)) {
    s_dragBright = true;
    setBrightFromX(px);
    return;
  }
  if (inRect(s_volHit, px, py)) {
    s_dragVol = true;
    setVolFromX(px);
    return;
  }
}

void onRelease() {
  if (s_dragBright || s_dragVol) {
    saveNvs();
  }
  s_dragBright = false;
  s_dragVol = false;
}

uint8_t brightness() { return s_bright; }
uint8_t volume() { return s_volume; }

void panelPortrait(int &x, int &y, int &w, int &h) {
  x = s_panel.x;
  y = s_panel.y;
  w = s_panel.w;
  h = s_panel.h;
}
void brightTrackPortrait(int &x, int &y, int &w, int &h) {
  x = s_brightTrack.x;
  y = s_brightTrack.y;
  w = s_brightTrack.w;
  h = s_brightTrack.h;
}
void volTrackPortrait(int &x, int &y, int &w, int &h) {
  x = s_volTrack.x;
  y = s_volTrack.y;
  w = s_volTrack.w;
  h = s_volTrack.h;
}
void closeBtnPortrait(int &x, int &y, int &w, int &h) {
  x = s_close.x;
  y = s_close.y;
  w = s_close.w;
  h = s_close.h;
}

} // namespace CrowPanelSetup

#endif
