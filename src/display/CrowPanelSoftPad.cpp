/*
 CrowPanel arcade soft pad + per-profile NVS persistence.
*/
#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include "CrowPanelSoftPad.h"
#include "C64LogoBitmap.h"
#include "CrowPanelHw.h"
#include "CrowPanelKeyLayout.h"
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
static constexpr int kProfileBarH = 40;

struct Circ {
  int cx, cy, r;
};
struct Rect {
  int x, y, w, h;
};

static Circ s_btn[kSlotCount];
static Rect s_body;
static Rect s_logo{};
static Rect s_profileBtn[kProfileCount]{};

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

static void layout() {
  const int gridBottom = crowKeyOriginY() + crowKeyGridH();
  const int top = gridBottom + 6;
  const int bottom = (int)CrowPanelHw::KB_PORTRAIT_H - 6;
  const int left = 6;
  const int right = (int)CrowPanelHw::KB_PORTRAIT_W - 6;
  const int areaH = bottom - top;
  const int areaW = right - left;
  if (areaH < 80 || areaW < 120) {
    memset(s_btn, 0, sizeof(s_btn));
    memset(s_profileBtn, 0, sizeof(s_profileBtn));
    s_body = {0, 0, 0, 0};
    s_logo = {0, 0, 0, 0};
    return;
  }

  s_body = {left, top, areaW, areaH};

  const int padIn = 8;
  const int profileTop = bottom - kProfileBarH;
  const int playBottom = profileTop - 4;
  const int playH = playBottom - top;
  const int halfW = (areaW - 3 * padIn) / 2;
  const int usableH = playH - 2 * padIn;
  const bool joy = (s_activeProfile == kProfileJoy);

  int r = usableH / 7;
  if (r > halfW / 7) {
    r = halfW / 7;
  }
  if (r < 16) {
    r = 16;
  }
  if (r > 40) {
    r = 40;
  }
  // JOY mode: fewer visible blue buttons — bump those a bit.
  // D-pad stays at normal size.
  int rJoy = (r * 5) / 4 + 2;
  if (rJoy > 48) {
    rJoy = 48;
  }
  const int rBlue = joy ? rJoy : r;
  const int gap = r / 5 + 4;
  const int gapJoy = rJoy / 5 + 4;

  // Left blue cluster: USER profiles keep original position; JOY shifts in.
  const int blueShift = joy ? (halfW / 5 + 10) : 0;
  const int lx0 = left + padIn + halfW / 2 + blueShift;
  const int ly0 = top + playH / 2;
  const int ldx = 2 * rBlue + (joy ? gapJoy : gap);
  const int ldy = 2 * rBlue + (joy ? gapJoy : gap);
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      int i = row * 3 + col;
      s_btn[i] = {lx0 - ldx + col * ldx, ly0 - ldy + row * ldy, rBlue};
    }
  }

  // Logo near top, centered over the D-pad column.
  const int logoW = C64_LOGO_W;
  const int logoH = C64_LOGO_H;
  const int rx0 = left + padIn + halfW + padIn + halfW / 2;
  const int logoCx = rx0;
  const int logoPy = top + 10;
  s_logo = {logoCx - logoW / 2, logoPy, logoW, logoH};

  // Right red D-pad — normal size, centered between logo and profile bank.
  const int logoBottom = logoPy + logoH;
  const int step = (7 * r) / 5 + gap;
  int ry0 = (logoBottom + profileTop) / 2;
  // Keep the whole cross inside the play band.
  if (ry0 - step - r < logoBottom + 4) {
    ry0 = logoBottom + 4 + step + r;
  }
  if (ry0 + step + r > profileTop - 4) {
    ry0 = profileTop - 4 - step - r;
  }
  s_btn[9] = {rx0, ry0 - step, r};
  s_btn[10] = {rx0, ry0 + step, r};
  s_btn[11] = {rx0 - step, ry0, r};
  s_btn[12] = {rx0 + step, ry0, r};

  // Bottom bank: JOY / USER1 / USER2 / USER3 — equal size.
  const int gapP = 6;
  const int btnW = (areaW - gapP * (kProfileCount + 1)) / kProfileCount;
  const int btnH = kProfileBarH - 8;
  const int by = profileTop + 4;
  for (int i = 0; i < kProfileCount; i++) {
    s_profileBtn[i] = {left + gapP + i * (btnW + gapP), by, btnW, btnH};
  }
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
  if (profile < 0 || profile >= kProfileCount || profile == s_activeProfile) {
    if (profile == s_activeProfile) {
      CrowPanelHw::requestKeyboardRedraw();
    }
    return;
  }
  syncProfileFromActive();
  s_activeProfile = profile;
  syncActiveFromProfile();
  layout(); // JOY uses larger buttons / recentered D-pad
  s_programMode = false;
  s_selected = -1;
  saveNvs();
  CrowPanelHw::requestKeyboardRedraw();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "profile %s",
                                     profileLabel(profile));
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
  for (int i = 0; i < kProfileCount; i++) {
    if (inRect(s_profileBtn[i], px, py)) {
      return static_cast<Hit>(kProfileHitBase + i);
    }
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

void profilePortrait(int profile, int &x, int &y, int &w, int &h) {
  if (profile < 0 || profile >= kProfileCount) {
    x = y = w = h = 0;
    return;
  }
  x = s_profileBtn[profile].x;
  y = s_profileBtn[profile].y;
  w = s_profileBtn[profile].w;
  h = s_profileBtn[profile].h;
}

void portraitToFb(int px, int py, int &fx, int &fy) {
  fx = (int)CrowPanelHw::KB_X0 + py;
  fy = (int)CrowPanelHw::PANEL_H - 1 - px;
}

} // namespace CrowPanelSoftPad

#endif
