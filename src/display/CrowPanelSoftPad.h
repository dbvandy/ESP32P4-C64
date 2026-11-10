/*
 CrowPanel arcade soft pad (beige rounded panel):
  - Left: 9 big blue programmable buttons (3×3)
  - Right: 4 big red programmable buttons (D-pad)
  - C64 rainbow logo above the D-pad
  - Bottom bank: JOY / USER1 / USER2 / USER3
  - PROG lives on the controls strip under D64
 Mappings persist per-profile in NVS and reload on boot.
*/
#ifndef CROWPANEL_SOFT_PAD_H
#define CROWPANEL_SOFT_PAD_H

#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include "../keyboard/CodeTripleDef.h"
#include <cstdint>

namespace CrowPanelSoftPad {

// 0..8 left (blue 3×3), 9..12 right arrows UP DN LT RT (red)
constexpr int kSlotCount = 13;
constexpr int kProgHit = 100;
constexpr int kProfileCount = 4;
constexpr int kProfileJoy = 0;
constexpr int kProfileUser1 = 1;
constexpr int kProfileUser2 = 2;
constexpr int kProfileUser3 = 3;
constexpr int kProfileHitBase = 110; // +0..3

// JOY visible blue slots: left-middle FIRE + center blank.
constexpr int kJoyFireSlot = 3;  // row1 col0
constexpr int kJoyBlankSlot = 4; // row1 col1

enum class Hit : int16_t {
  None = -1,
  Slot0 = 0,
  Slot1 = 1,
  Slot2 = 2,
  Slot3 = 3,
  Slot4 = 4,
  Slot5 = 5,
  Slot6 = 6,
  Slot7 = 7,
  Slot8 = 8,
  ArrowUp = 9,
  ArrowDown = 10,
  ArrowLeft = 11,
  ArrowRight = 12,
  Prog = kProgHit,
  ProfileJoy = kProfileHitBase + 0,
  ProfileUser1 = kProfileHitBase + 1,
  ProfileUser2 = kProfileHitBase + 2,
  ProfileUser3 = kProfileHitBase + 3,
};

struct Slot {
  uint8_t dc00 = 0xff;
  uint8_t dc01 = 0xff;
  uint8_t modifier = 0;
  char label[6] = "--";
  bool assigned() const { return dc00 != 0xff || dc01 != 0xff; }
  CodeTripleS code() const { return {dc00, dc01, modifier}; }
};

inline bool isArrowSlot(int i) { return i >= 9 && i <= 12; }
inline bool isBlueSlot(int i) { return i >= 0 && i <= 8; }
inline bool isProfileHit(int id) {
  return id >= kProfileHitBase && id < kProfileHitBase + kProfileCount;
}

void init();
Hit hitTest(int touchX, int touchY);

bool programMode();
int selectedSlot(); // 0..12 while waiting for a keyboard key, else -1

void onProg();
void onSelectSlot(int slot);
bool assignFromKey(const char *label, const CodeTripleS &code);

const Slot &slot(int i);
bool slotVisible(int i); // false in JOY for hidden blue buttons

int activeProfile(); // 0..3
void setProfile(int profile);
const char *profileLabel(int profile);

int highlight();           // lit button id, or -1
void setHighlight(int id); // starts a ~0.25s under-light
void tick();               // expire timed glow

// Portrait geometry for drawing (centers + radii).
void bodyPortrait(int &x, int &y, int &w, int &h);
void buttonPortrait(int i, int &cx, int &cy, int &r);
void logoPortrait(int &cx, int &cy, int &w, int &h);
void profilePortrait(int profile, int &x, int &y, int &w, int &h);

// Portrait → landscape FB point (for centers).
void portraitToFb(int px, int py, int &fx, int &fy);

} // namespace CrowPanelSoftPad

#endif
#endif
