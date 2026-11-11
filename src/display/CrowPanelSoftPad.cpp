/*
 CrowPanel arcade soft pad + per-profile NVS persistence.
*/
#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include "CrowPanelSoftPad.h"
#include "C64LogoBitmap.h"
#include "CrowPanelHw.h"
#include "CrowPanelKeyLayout.h"
#include "../ExtCmd.h"
#include "../ExtCmdQueue.h"
#include "../keyboard/C64Keycodes.h"
#include "../platform/PlatformManager.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

namespace CrowPanelSoftPad {
namespace {

static const char *TAG = "SoftPad";
static constexpr uint32_t kMapMagic = 0x50414434; // "PAD4" profiles

static Slot s_slots[kSlotCount];
static Slot s_profiles[kProfileCount][kSlotCount];
static int s_activeProfile = kProfileJoy;
static bool s_programMode = false;
static int s_selected = -1;
static int s_highlight = -1;
static uint32_t s_glowUntilMs = 0;
static constexpr uint32_t kGlowMs = 250;
static bool s_selectActive = false;

struct Circ {
  int cx, cy, r;
};
struct Rect {
  int x, y, w, h;
};

static Circ s_btn[kSlotCount];
static Rect s_body;
static Rect s_logo{};
static Rect s_selectPanel{};
static Rect s_selectChoice[kProfileCount]{};
static Rect s_selectClose{};

#pragma pack(push, 1)
struct MapBlob {
  uint32_t magic;
  uint8_t active;
  uint8_t _pad[3];
  Slot profiles[kProfileCount][kSlotCount];
};
#pragma pack(pop)

static void clearSlot(Slot &s) {
  s.dc00 = 0xff;
  s.dc01 = 0xff;
  s.modifier = 0;
  memset(s.label, 0, sizeof(s.label));
}

static void setSlot(Slot &s, const CodeTripleS &code, const char *lab) {
  s.dc00 = code.dc00;
  s.dc01 = code.dc01;
  s.modifier = code.modifier;
  memset(s.label, 0, sizeof(s.label));
  if (lab && lab[0]) {
    strncpy(s.label, lab, sizeof(s.label) - 1);
  }
}

static void fillUserDefaults(Slot *dst) {
  const CodeTripleS defs[kSlotCount] = {
      C64_KEYCODE_Z,     C64_KEYCODE_X,     C64_KEYCODE_C,
      C64_KEYCODE_A,     C64_KEYCODE_S,     C64_KEYCODE_D,
      C64_KEYCODE_SPACE, C64_KEYCODE_RETURN, C64_KEYCODE_F1,
      C64_KEYCODE_UP,    C64_KEYCODE_DOWN,  C64_KEYCODE_LEFT,
      C64_KEYCODE_RIGHT};
  const char *labs[kSlotCount] = {"Z",   "X",   "C",  "A",  "S",  "D",
                                  "SPC", "RET", "F1", "UP", "DN", "LT",
                                  "RT"};
  for (int i = 0; i < kSlotCount; i++) {
    setSlot(dst[i], defs[i], labs[i]);
  }
}

static void fillJoyDefaults(Slot *dst) {
  for (int i = 0; i < kSlotCount; i++) {
    clearSlot(dst[i]);
  }
  setSlot(dst[kJoyFireSlot], C64_KEYCODE_SPACE, "FIRE");
  // kJoyBlankSlot stays cleared (visible but blank).
  setSlot(dst[9], C64_KEYCODE_UP, "UP");
  setSlot(dst[10], C64_KEYCODE_DOWN, "DN");
  setSlot(dst[11], C64_KEYCODE_LEFT, "LT");
  setSlot(dst[12], C64_KEYCODE_RIGHT, "RT");
}

static void syncActiveFromProfile() {
  memcpy(s_slots, s_profiles[s_activeProfile], sizeof(s_slots));
}

static void syncProfileFromActive() {
  memcpy(s_profiles[s_activeProfile], s_slots, sizeof(s_slots));
}

static void saveNvs() {
  Preferences prefs;
  if (!prefs.begin("softpad", false)) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "NVS begin failed");
    return;
  }
  syncProfileFromActive();
  MapBlob blob{};
  blob.magic = kMapMagic;
  blob.active = (uint8_t)s_activeProfile;
  memcpy(blob.profiles, s_profiles, sizeof(s_profiles));
  prefs.putBytes("map4", &blob, sizeof(blob));
  prefs.end();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "saved profiles (active=%d)",
                                     s_activeProfile);
}

static void loadNvs() {
  fillJoyDefaults(s_profiles[kProfileJoy]);
  fillUserDefaults(s_profiles[kProfileUser1]);
  fillUserDefaults(s_profiles[kProfileUser2]);
  fillUserDefaults(s_profiles[kProfileUser3]);
  s_activeProfile = kProfileJoy;

  Preferences prefs;
  if (!prefs.begin("softpad", true)) {
    syncActiveFromProfile();
    return;
  }
  MapBlob blob{};
  size_t n = prefs.getBytesLength("map4");
  if (n == sizeof(blob) && prefs.getBytes("map4", &blob, sizeof(blob)) == n &&
      blob.magic == kMapMagic) {
    memcpy(s_profiles, blob.profiles, sizeof(s_profiles));
    s_activeProfile = blob.active < kProfileCount ? blob.active : kProfileJoy;
    PlatformManager::getInstance().log(LOG_INFO, TAG, "loaded profiles");
  }
  prefs.end();
  syncActiveFromProfile();
}

static void pushJoyPortMode(bool joy1) {
  ExtCmdQueue::ExternalCmd extcmd{};
  extcmd.cmd = joy1 ? ExtCmd::KBJOYSTICKMODE1 : ExtCmd::KBJOYSTICKMODEOFF;
  memset(extcmd.param, 0, sizeof(extcmd.param));
  ExtCmdQueue::getInstance().push(extcmd);
}

static void layoutSelect() {
  const int pw = CrowPanelHw::KB_PORTRAIT_W;
  const int ph = CrowPanelHw::KB_PORTRAIT_H;
  const int cardW = 300;
  const int cardH = 260;
  s_selectPanel = {(pw - cardW) / 2, (ph - cardH) / 2, cardW, cardH};

  const int pad = 16;
  const int titleBand = 40;
  const int cr = 14;
  s_selectClose = {s_selectPanel.x + s_selectPanel.w - 34, s_selectPanel.y + 8,
                   cr * 2, cr * 2};

  const int gridX = s_selectPanel.x + pad;
  const int gridY = s_selectPanel.y + titleBand;
  const int gridW = s_selectPanel.w - 2 * pad;
  const int gridH = s_selectPanel.h - titleBand - pad;
  const int gap = 12;
  const int cellW = (gridW - gap) / 2;
  const int cellH = (gridH - gap) / 2;
  for (int i = 0; i < kProfileCount; i++) {
    const int col = i % 2;
    const int row = i / 2;
    s_selectChoice[i] = {gridX + col * (cellW + gap),
                         gridY + row * (cellH + gap), cellW, cellH};
  }
}

static void layout() {
  // Soft pad on top of portrait band (keyboard sits below).
  const int top = crowSoftPadTop();
  const int bottom = crowSoftPadBottom();
  const int left = 6;
  const int right = (int)CrowPanelHw::KB_PORTRAIT_W - 6;
  const int areaH = bottom - top;
  const int areaW = right - left;
  if (areaH < 80 || areaW < 120) {
    memset(s_btn, 0, sizeof(s_btn));
    s_body = {0, 0, 0, 0};
    s_logo = {0, 0, 0, 0};
    return;
  }

  s_body = {left, top, areaW, areaH};

  const int padIn = 8;
  // Full soft-pad height is play area (profile bank moved to PAD window).
  const int playH = areaH;
  const int halfW = (areaW - 3 * padIn) / 2;
  const int usableH = playH - 2 * padIn;
  const bool joy = (s_activeProfile == kProfileJoy);

  // Bigger D-pad: use more of the freed vertical space.
  int r = usableH / 6;
  if (r > halfW / 6) {
    r = halfW / 6;
  }
  if (r < 18) {
    r = 18;
  }
  if (r > 56) {
    r = 56;
  }
  // JOY blue buttons keep current size; USER blues −10%.
  int rJoy = (r * 5) / 4 + 2;
  if (rJoy > 60) {
    rJoy = 60;
  }
  const int rUser = (r * 9) / 10;
  const int rBlue = joy ? rJoy : rUser;
  // D-pad (all profiles) −10%.
  const int rPad = (r * 9) / 10;
  const int gap = rPad / 5 + 4;
  const int gapJoy = rJoy / 5 + 4;
  const int gapUser = rUser / 5 + 4;

  // Left blue cluster: USER nudged right onto the panel; JOY shifts in more.
  const int blueShift = joy ? (halfW / 5 + 10) : 15;
  const int lx0 = left + padIn + halfW / 2 + blueShift;
  const int ly0 = top + playH / 2;
  const int ldx = 2 * rBlue + (joy ? gapJoy : gapUser);
  const int ldy = 2 * rBlue + (joy ? gapJoy : gapUser);
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      int i = row * 3 + col;
      s_btn[i] = {lx0 - ldx + col * ldx, ly0 - ldy + row * ldy, rBlue};
    }
  }

  // Logo near top, centered over the D-pad column (+10px down, +10% size).
  const int logoW = (C64_LOGO_W * 11) / 10;
  const int logoH = (C64_LOGO_H * 11) / 10;
  const int rx0 = left + padIn + halfW + padIn + halfW / 2;
  const int logoCx = rx0;
  const int logoPy = top + 18;
  s_logo = {logoCx - logoW / 2, logoPy, logoW, logoH};

  // Right red D-pad — centered under logo (10% smaller than base r).
  const int logoBottom = logoPy + logoH;
  const int playBottom = bottom;
  const int step = (7 * rPad) / 5 + gap;
  int ry0 = (logoBottom + playBottom) / 2;
  if (ry0 - step - rPad < logoBottom + 4) {
    ry0 = logoBottom + 4 + step + rPad;
  }
  if (ry0 + step + rPad > playBottom - 4) {
    ry0 = playBottom - 4 - step - rPad;
  }
  s_btn[9] = {rx0, ry0 - step, rPad};
  s_btn[10] = {rx0, ry0 + step, rPad};
  s_btn[11] = {rx0 - step, ry0, rPad};
  s_btn[12] = {rx0 + step, ry0, rPad};

  layoutSelect();
}

static bool inCircle(const Circ &c, int px, int py) {
  if (c.r <= 0) {
    return false;
  }
  const int dx = px - c.cx;
  const int dy = py - c.cy;
  return dx * dx + dy * dy <= c.r * c.r;
}

static bool inRect(const Rect &r, int px, int py) {
  return r.w > 0 && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static void portraitFromTouch(int tx, int ty, int &px, int &py) {
  px = (int)CrowPanelHw::PANEL_H - 1 - ty;
  py = tx - (int)CrowPanelHw::KB_X0;
}

} // namespace

void init() {
  loadNvs();
  layout();
  s_programMode = false;
  s_selected = -1;
  s_highlight = -1;
  // Default / restored JOY profile → CIA port 1 virtual stick.
  if (s_activeProfile == kProfileJoy) {
    pushJoyPortMode(true);
  }
}

void setHighlight(int id) {
  s_highlight = id;
  if (id >= 0) {
    s_glowUntilMs = millis() + kGlowMs;
  } else {
    s_glowUntilMs = 0;
  }
  CrowPanelHw::requestKeyboardRedraw();
}
int highlight() { return s_highlight; }

void tick() {
  if (s_highlight < 0 || s_glowUntilMs == 0) {
    return;
  }
  if (s_programMode && s_selected >= 0 && s_highlight == s_selected) {
    return;
  }
  if ((int32_t)(millis() - s_glowUntilMs) >= 0) {
    s_highlight = -1;
    s_glowUntilMs = 0;
    CrowPanelHw::requestKeyboardRedraw();
  }
}

bool programMode() { return s_programMode; }
int selectedSlot() { return s_selected; }

const Slot &slot(int i) {
  static Slot empty;
  if (i < 0 || i >= kSlotCount) {
    return empty;
  }
  return s_slots[i];
}

bool slotVisible(int i) {
  if (i < 0 || i >= kSlotCount) {
    return false;
  }
  if (s_activeProfile != kProfileJoy) {
    return true;
  }
  if (isArrowSlot(i)) {
    return true;
  }
  return i == kJoyFireSlot || i == kJoyBlankSlot;
}

int activeProfile() { return s_activeProfile; }

const char *profileLabel(int profile) {
  switch (profile) {
  case kProfileJoy:
    return "JOY";
  case kProfileUser1:
    return "USER1";
  case kProfileUser2:
    return "USER2";
  case kProfileUser3:
    return "USER3";
  default:
    return "?";
  }
}

void setProfile(int profile) {
  if (profile < 0 || profile >= kProfileCount) {
    return;
  }
  if (profile == s_activeProfile) {
    s_selectActive = false;
    CrowPanelHw::requestKeyboardRedraw();
    return;
  }
  const int prev = s_activeProfile;
  syncProfileFromActive();
  s_activeProfile = profile;
  syncActiveFromProfile();
  layout(); // JOY uses larger buttons / recentered D-pad
  s_programMode = false;
  s_selected = -1;
  s_selectActive = false;
  saveNvs();
  CrowPanelHw::requestKeyboardRedraw();
  // JOY profile drives CIA Joy1; leaving it releases the stick port.
  if (profile == kProfileJoy && prev != kProfileJoy) {
    pushJoyPortMode(true);
  } else if (prev == kProfileJoy && profile != kProfileJoy) {
    pushJoyPortMode(false);
  }
  PlatformManager::getInstance().log(LOG_INFO, TAG, "profile %s",
                                     profileLabel(profile));
}

bool selectActive() { return s_selectActive; }

void openSelect() {
  layoutSelect();
  s_selectActive = true;
  CrowPanelHw::requestKeyboardRedraw();
}

void closeSelect() {
  if (!s_selectActive) {
    return;
  }
  s_selectActive = false;
  CrowPanelHw::requestKeyboardRedraw();
}

void onSelectTouch(int touchX, int touchY) {
  if (!s_selectActive || touchX < (int)CrowPanelHw::KB_X0) {
    return;
  }
  int px, py;
  portraitFromTouch(touchX, touchY, px, py);
  if (inRect(s_selectClose, px, py)) {
    closeSelect();
    return;
  }
  for (int i = 0; i < kProfileCount; i++) {
    if (inRect(s_selectChoice[i], px, py)) {
      setProfile(i);
      return;
    }
  }
}

void onProg() {
  if (s_programMode) {
    s_programMode = false;
    s_selected = -1;
    PlatformManager::getInstance().log(LOG_INFO, TAG, "program cancel");
  } else {
    s_programMode = true;
    s_selected = -1;
    PlatformManager::getInstance().log(LOG_INFO, TAG,
                                       "program: tap a pad button");
  }
  CrowPanelHw::requestKeyboardRedraw();
}

void onSelectSlot(int slotIdx) {
  if (!s_programMode || slotIdx < 0 || slotIdx >= kSlotCount) {
    return;
  }
  if (!slotVisible(slotIdx)) {
    return;
  }
  s_selected = slotIdx;
  PlatformManager::getInstance().log(
      LOG_INFO, TAG, "program: tap a keyboard key for slot %d", slotIdx);
  CrowPanelHw::requestKeyboardRedraw();
}

bool assignFromKey(const char *label, const CodeTripleS &code) {
  if (!s_programMode || s_selected < 0 || s_selected >= kSlotCount) {
    return false;
  }
  Slot &s = s_slots[s_selected];
  s.dc00 = code.dc00;
  s.dc01 = code.dc01;
  s.modifier = code.modifier;
  memset(s.label, 0, sizeof(s.label));
  if (label && label[0]) {
    strncpy(s.label, label, sizeof(s.label) - 1);
  } else {
    strncpy(s.label, "?", sizeof(s.label) - 1);
  }
  saveNvs();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "slot %d = %s", s_selected,
                                     s.label);
  s_programMode = false;
  s_selected = -1;
  CrowPanelHw::requestKeyboardRedraw();
  return true;
}

Hit hitTest(int touchX, int touchY) {
  if (touchX < (int)CrowPanelHw::KB_X0) {
    return Hit::None;
  }
  int px, py;
  portraitFromTouch(touchX, touchY, px, py);
  if (px < 0 || py < 0 || px >= (int)CrowPanelHw::KB_PORTRAIT_W ||
      py >= (int)CrowPanelHw::KB_PORTRAIT_H) {
    return Hit::None;
  }
  for (int i = 0; i < kSlotCount; i++) {
    if (!slotVisible(i)) {
      continue;
    }
    if (inCircle(s_btn[i], px, py)) {
      return static_cast<Hit>(i);
    }
  }
  return Hit::None;
}

void bodyPortrait(int &x, int &y, int &w, int &h) {
  x = s_body.x;
  y = s_body.y;
  w = s_body.w;
  h = s_body.h;
}

void buttonPortrait(int i, int &cx, int &cy, int &r) {
  if (i < 0 || i >= kSlotCount || !slotVisible(i)) {
    cx = cy = r = 0;
    return;
  }
  cx = s_btn[i].cx;
  cy = s_btn[i].cy;
  r = s_btn[i].r;
}


void logoPortrait(int &cx, int &cy, int &w, int &h) {
  cx = s_logo.x + s_logo.w / 2;
  cy = s_logo.y + s_logo.h / 2;
  w = s_logo.w;
  h = s_logo.h;
}

void selectPanelPortrait(int &x, int &y, int &w, int &h) {
  x = s_selectPanel.x;
  y = s_selectPanel.y;
  w = s_selectPanel.w;
  h = s_selectPanel.h;
}

void selectChoicePortrait(int profile, int &x, int &y, int &w, int &h) {
  if (profile < 0 || profile >= kProfileCount) {
    x = y = w = h = 0;
    return;
  }
  x = s_selectChoice[profile].x;
  y = s_selectChoice[profile].y;
  w = s_selectChoice[profile].w;
  h = s_selectChoice[profile].h;
}

void selectClosePortrait(int &x, int &y, int &w, int &h) {
  x = s_selectClose.x;
  y = s_selectClose.y;
  w = s_selectClose.w;
  h = s_selectClose.h;
}

void portraitToFb(int px, int py, int &fx, int &fy) {
  fx = (int)CrowPanelHw::KB_X0 + py;
  fy = (int)CrowPanelHw::PANEL_H - 1 - px;
}

} // namespace CrowPanelSoftPad

#endif
