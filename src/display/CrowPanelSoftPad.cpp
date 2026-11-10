/*
 CrowPanel arcade soft pad + NVS persistence (13 programmable slots).
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
static constexpr uint32_t kMapMagic = 0x50414433; // "PAD3" (13 slots)

static Slot s_slots[kSlotCount];
static bool s_programMode = false;
static int s_selected = -1;
static int s_highlight = -1;
static uint32_t s_glowUntilMs = 0;
static constexpr uint32_t kGlowMs = 250;

struct Circ {
  int cx, cy, r;
};
struct Rect {
  int x, y, w, h;
};

static Circ s_btn[kSlotCount];
static Circ s_prog;
static Rect s_body;
static Rect s_logo{}; // portrait rect for C64 mark above D-pad

#pragma pack(push, 1)
struct MapBlob {
  uint32_t magic;
  Slot slots[kSlotCount];
};
#pragma pack(pop)

static void setDefaultSlots() {
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
    s_slots[i].dc00 = defs[i].dc00;
    s_slots[i].dc01 = defs[i].dc01;
    s_slots[i].modifier = defs[i].modifier;
    strncpy(s_slots[i].label, labs[i], sizeof(s_slots[i].label) - 1);
    s_slots[i].label[sizeof(s_slots[i].label) - 1] = '\0';
  }
}

static void saveNvs() {
  Preferences prefs;
  if (!prefs.begin("softpad", false)) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "NVS begin failed");
    return;
  }
  MapBlob blob{};
  blob.magic = kMapMagic;
  memcpy(blob.slots, s_slots, sizeof(s_slots));
  prefs.putBytes("map3", &blob, sizeof(blob));
  prefs.end();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "saved pad map (13 slots)");
}

static void loadNvs() {
  setDefaultSlots();
  Preferences prefs;
  if (!prefs.begin("softpad", true)) {
    return;
  }
  MapBlob blob{};
  size_t n = prefs.getBytesLength("map3");
  if (n == sizeof(blob) && prefs.getBytes("map3", &blob, sizeof(blob)) == n &&
      blob.magic == kMapMagic) {
    memcpy(s_slots, blob.slots, sizeof(s_slots));
    PlatformManager::getInstance().log(LOG_INFO, TAG, "loaded pad map");
  }
  prefs.end();
}

static void layout() {
  const int gridBottom = crowKeyOriginY() + crowKeyGridH();
  const int top = gridBottom + 6;
  const int bottom = (int)CrowPanelHw::KB_PORTRAIT_H - 6;
  const int left = 6;
  const int right = (int)CrowPanelHw::KB_PORTRAIT_W - 6;
  const int areaH = bottom - top;
  const int areaW = right - left;
  if (areaH < 60 || areaW < 120) {
    memset(s_btn, 0, sizeof(s_btn));
    s_prog = {0, 0, 0};
    s_body = {0, 0, 0, 0};
    s_logo = {0, 0, 0, 0};
    return;
  }

  // Beige rounded panel body (grey margin drawn around it).
  s_body = {left, top, areaW, areaH};

  // Button radius: pack as large as possible (3×3 left + D-pad cross).
  const int padIn = 8;
  const int halfW = (areaW - 3 * padIn) / 2;
  const int usableH = areaH - 2 * padIn;
  // 3 rows / 3 cols need ~3*(2r) + 2*gap.
  int r = usableH / 7;
  if (r > halfW / 7) {
    r = halfW / 7;
  }
  if (r < 18) {
    r = 18;
  }
  if (r > 42) {
    r = 42;
  }
  const int gap = r / 5 + 4;

  // Left blue cluster (3×3), centered in left half of body.
  const int lx0 = left + padIn + halfW / 2;
  const int ly0 = top + areaH / 2;
  const int ldx = 2 * r + gap;
  const int ldy = 2 * r + gap;
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      int i = row * 3 + col;
      s_btn[i] = {lx0 - ldx + col * ldx, ly0 - ldy + row * ldy, r};
    }
  }

  // Right red D-pad cross — nudged down a bit for the C64 logo above.
  const int rx0 = left + padIn + halfW + padIn + halfW / 2;
  const int ry0 = top + areaH / 2 + 12;
  const int step = (7 * r) / 5 + gap;
  s_btn[9] = {rx0, ry0 - step, r};  // UP
  s_btn[10] = {rx0, ry0 + step, r}; // DN
  s_btn[11] = {rx0 - step, ry0, r}; // LT
  s_btn[12] = {rx0 + step, ry0, r}; // RT

  // C64 logo bitmap above the UP button (drawn 90° CCW → vertical pill).
  // Portrait (logoW × logoH) maps to on-screen (logoH × logoW).
  const int logoW = C64_LOGO_W;
  const int logoH = C64_LOGO_H;
  const int upLeft = (ry0 - step) - r; // smaller py = toward keys / "above"
  int logoPy = upLeft - 6 - logoH;
  if (logoPy < top + padIn) {
    logoPy = top + padIn;
  }
  s_logo = {rx0 - logoW / 2, logoPy, logoW, logoH};

  // Small PROG in upper-right of the body.
  const int pr = (r * 2) / 5;
  s_prog = {right - padIn - pr - 4, top + padIn + pr + 2, pr < 10 ? 10 : pr};
}

static bool inCircle(const Circ &c, int px, int py) {
  if (c.r <= 0) {
    return false;
  }
  const int dx = px - c.cx;
  const int dy = py - c.cy;
  return dx * dx + dy * dy <= c.r * c.r;
}

static void portraitFromTouch(int tx, int ty, int &px, int &py) {
  px = (int)CrowPanelHw::PANEL_H - 1 - ty;
  py = tx - (int)CrowPanelHw::KB_X0;
}

} // namespace

void init() {
  layout();
  loadNvs();
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
  // Keep glow while programming a selected slot.
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
  // Prefer PROG if overlapping (it's small / upper-right).
  if (inCircle(s_prog, px, py)) {
    return Hit::Prog;
  }
  for (int i = 0; i < kSlotCount; i++) {
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
  if (i < 0 || i >= kSlotCount) {
    cx = cy = r = 0;
    return;
  }
  cx = s_btn[i].cx;
  cy = s_btn[i].cy;
  r = s_btn[i].r;
}

void progPortrait(int &cx, int &cy, int &r) {
  cx = s_prog.cx;
  cy = s_prog.cy;
  r = s_prog.r;
}

void logoPortrait(int &cx, int &cy, int &w, int &h) {
  cx = s_logo.x + s_logo.w / 2;
  cy = s_logo.y + s_logo.h / 2;
  w = s_logo.w;
  h = s_logo.h;
}

void portraitToFb(int px, int py, int &fx, int &fy) {
  // Same transform as crowKeyRect: portrait → landscape KB strip.
  fx = (int)CrowPanelHw::KB_X0 + py;
  fy = (int)CrowPanelHw::PANEL_H - 1 - px;
}

} // namespace CrowPanelSoftPad

#endif
