/*
 On-screen C64 keyboard for CrowPanel (touch → CIA matrix codes).
 Soft-pad / matrix keys: hold 0.5s to auto-repeat.
*/
#include "../Config.h"
#ifdef USE_CROWPANEL_TOUCH_KEYBOARD

#include "CrowPanelTouchKB.h"
#include "board_config.h"
#include "esp_panel_board_custom_conf.h"
#include "esp_panel_drivers_conf.h"
#include "esp_utils_conf.h"
#include "../ExtCmd.h"
#include "../ExtCmdQueue.h"
#include "../display/CrowPanelDirBrowser.h"
#include "../display/CrowPanelFileBrowser.h"
#include "../display/CrowPanelHw.h"
#include "../display/CrowPanelKeyLayout.h"
#include "../display/CrowPanelSetup.h"
#include "../display/CrowPanelSoftPad.h"
#include "../platform/PlatformManager.h"
#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <cstring>

using namespace esp_panel::drivers;

static const char *TAG = "CrowTouchKB";

void CrowPanelTouchKB::clearMatrix() {
  sentdc00.store(0xff, std::memory_order_release);
  sentdc01.store(0xff, std::memory_order_release);
}

void CrowPanelTouchKB::clearJoy() {
  joyvalue.store(0xff, std::memory_order_release);
}

int CrowPanelTouchKB::joyBitForSlot(int slot) {
  // CIA joystick active-low: bit0 up, bit1 down, bit2 left, bit3 right, bit4 fire.
  if (slot == CrowPanelSoftPad::kJoyFireSlot) {
    return 4;
  }
  if (slot == 9) {
    return 0; // UP
  }
  if (slot == 10) {
    return 1; // DOWN
  }
  if (slot == 11) {
    return 2; // LEFT
  }
  if (slot == 12) {
    return 3; // RIGHT
  }
  return -1;
}

void CrowPanelTouchKB::applyModsOnly() {
  uint8_t mods = sticky | (shiftLock ? 1 : 0);
  shiftctrlcode.store(mods, std::memory_order_release);
  CrowPanelHw::setStickyMods(mods);
}

void CrowPanelTouchKB::startPulse(const CodeTripleS &code) {
  uint8_t mods =
      (uint8_t)(sticky | (shiftLock ? 1 : 0) | (code.modifier & 0x07));
  sentdc00.store(code.dc00, std::memory_order_release);
  sentdc01.store(code.dc01, std::memory_order_release);
  shiftctrlcode.store(mods, std::memory_order_release);
  pulseLeft = kPulseCycles;
  // One-shot sticky modifiers (keep shift-lock)
  sticky = 0;
  CrowPanelHw::setStickyMods(shiftLock ? 1 : 0);
}

void CrowPanelTouchKB::clearHold() {
  holdRepeat = false;
  holdIsPad = false;
  holdIsJoy = false;
  holdId = -1;
  holdStartMs = 0;
  lastFireMs = 0;
}

void CrowPanelTouchKB::beginHold(bool isPad, int id, const CodeTripleS &code) {
  holdRepeat = true;
  holdIsPad = isPad;
  holdIsJoy = false;
  holdId = id;
  holdCode = code;
  holdStartMs = millis();
  lastFireMs = holdStartMs;
}

bool CrowPanelTouchKB::updateJoyFromPoints(const void *ptsVoid, int n) {
  const auto *pts = static_cast<const TouchPoint *>(ptsVoid);
  uint8_t joy = 0xff;
  int lastHit = -1;
  for (int i = 0; i < n; i++) {
    CrowPanelSoftPad::Hit sh =
        CrowPanelSoftPad::hitTest(pts[i].x, pts[i].y);
    const int bit = joyBitForSlot((int)sh);
    if (bit < 0) {
      continue;
    }
    joy = (uint8_t)(joy & ~(1u << bit));
    lastHit = (int)sh;
  }
  joyvalue.store(joy, std::memory_order_release);
  if (lastHit >= 0) {
    CrowPanelSoftPad::setHighlight(lastHit);
    holdIsJoy = true;
    holdIsPad = true;
    holdRepeat = true;
    holdId = lastHit;
    return true;
  }
  return false;
}

void CrowPanelTouchKB::pollHoldRepeat(int touchX, int touchY) {
  if (!holdRepeat || holdId < 0) {
    return;
  }
  // Joy diagonals are refreshed every scan via updateJoyFromPoints.
  if (holdIsJoy) {
    return;
  }
  bool stillOn = false;
  if (holdIsPad) {
    CrowPanelSoftPad::Hit sh = CrowPanelSoftPad::hitTest(touchX, touchY);
    stillOn = ((int)sh == holdId);
  } else {
    stillOn = (crowHitTest(touchX, touchY) == holdId);
  }
  if (!stillOn) {
    return;
  }
  const uint32_t now = millis();
  if (now - holdStartMs < kHoldRepeatDelayMs) {
    return;
  }
  if (now - lastFireMs < kHoldRepeatRateMs) {
    return;
  }
  // Only re-fire once the previous pulse has cleared (avoid stuck matrix).
  if (pulseLeft > 0) {
    return;
  }
  startPulse(holdCode);
  lastFireMs = now;
  if (holdIsPad) {
    CrowPanelSoftPad::setHighlight(holdId);
  } else {
    CrowPanelHw::setHighlight(holdId);
    activeKey = holdId;
  }
}

void CrowPanelTouchKB::init() {
  clearMatrix();
  clearJoy();
  shiftctrlcode.store(0, std::memory_order_release);
  sticky = 0;
  shiftLock = false;
  fingerDown = false;
  awaitingRelease = false;
  releaseQuiet = 0;
  pulseLeft = 0;
  activeKey = -1;
  clearHold();
  CrowPanelFileBrowser::close();
  CrowPanelDirBrowser::close();
  ExtCmdQueue::getInstance().clear();
  CrowPanelHw::setStickyMods(0);
  CrowPanelHw::setHighlight(-1);
  CrowPanelSoftPad::init();
  CrowPanelSetup::init();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "touch keyboard ready");
}

void CrowPanelTouchKB::scanKeyboard() {
  CrowPanelSoftPad::tick(); // expire 0.25s arcade under-lights

  // Count down a one-shot matrix pulse even if finger stays down
  if (pulseLeft > 0) {
    pulseLeft--;
    if (pulseLeft == 0) {
      clearMatrix();
      applyModsOnly();
      CrowPanelHw::setHighlight(-1);
      activeKey = -1;
    }
  }

  auto *tp = CrowPanelHw::touch();
  if (!tp) {
    return;
  }

  // GT911: up to 5 contacts — need ≥2 for joystick diagonals.
  static constexpr int kMaxPts = 5;
  TouchPoint pts[kMaxPts];
  int n = tp->readPoints(pts, kMaxPts, 0);
  const bool joyProfile =
      CrowPanelSoftPad::activeProfile() == CrowPanelSoftPad::kProfileJoy;

  // Rebuild Joy1 bits every scan from all fingers on D-pad/FIRE.
  bool joyActive = false;
  if (joyProfile) {
    if (n > 0) {
      joyActive = updateJoyFromPoints(pts, n);
    } else {
      clearJoy();
    }
  }

  if (n <= 0) {
    if (CrowPanelSetup::active()) {
      CrowPanelSetup::onRelease();
    }
    // Require a sustained lift so a mid-press dropout doesn't re-arm.
    if (awaitingRelease || fingerDown) {
      if (++releaseQuiet >= kReleaseQuietScans) {
        fingerDown = false;
        awaitingRelease = false;
        releaseQuiet = 0;
        if (holdIsJoy) {
          clearJoy();
        }
        clearHold();
      }
    } else {
      releaseQuiet = 0;
    }
    if (pulseLeft == 0) {
      CrowPanelHw::setHighlight(-1);
      // Soft-pad glow is timed (0.25s) — do not clear it on finger-up.
      activeKey = -1;
    }
    return;
  }

  releaseQuiet = 0;

  // SETUP sliders need continuous drag while the finger is down.
  if (CrowPanelSetup::active()) {
    clearHold();
    CrowPanelSetup::onTouch(pts[0].x, pts[0].y);
    fingerDown = true;
    awaitingRelease = true;
    return;
  }

  // Soft-pad JOY/USER picker (Setup-style window).
  if (CrowPanelSoftPad::selectActive()) {
    clearHold();
    CrowPanelSoftPad::onSelectTouch(pts[0].x, pts[0].y);
    fingerDown = true;
    awaitingRelease = true;
    return;
  }

  // Joy diagonals already updated above; keep contact state without blocking
  // a second finger that lands on another D-pad button.
  if (joyActive) {
    fingerDown = true;
    awaitingRelease = true;
    CrowPanelHw::setHighlight(-1);
    // Fall through only if some point is NOT a joy control (keys).
    bool onlyJoy = true;
    for (int i = 0; i < n; i++) {
      CrowPanelSoftPad::Hit sh =
          CrowPanelSoftPad::hitTest(pts[i].x, pts[i].y);
      if (sh == CrowPanelSoftPad::Hit::None) {
        onlyJoy = false;
        break;
      }
      if (joyBitForSlot((int)sh) < 0) {
        onlyJoy = false;
        break;
      }
    }
    if (onlyJoy) {
      return;
    }
  }

  // Still contacting after we already fired this press — maybe auto-repeat.
  if (awaitingRelease && !joyActive) {
    pollHoldRepeat(pts[0].x, pts[0].y);
    return;
  }

  // Rising edge only (finger was up, now down) — skip if joy already tracking.
  if (fingerDown && !joyActive) {
    return;
  }
  if (!fingerDown) {
    fingerDown = true;
    awaitingRelease = true; // one event per contact (non-joy)
  }

  // Directory (LIST) window intercepts touches while open.
  if (CrowPanelDirBrowser::active()) {
    using CrowPanelDirBrowser::Hit;
    Hit h = CrowPanelDirBrowser::hitTest(pts[0].x, pts[0].y);
    if (h == Hit::Close) {
      CrowPanelDirBrowser::close();
    } else if (h == Hit::Up) {
      CrowPanelDirBrowser::scrollBy(-1);
    } else if (h == Hit::Down) {
      CrowPanelDirBrowser::scrollBy(1);
    }
    CrowPanelHw::setHighlight(-1);
    return;
  }

  // D64 file browser intercepts touches while open.
  if (CrowPanelFileBrowser::active()) {
    using CrowPanelFileBrowser::Hit;
    Hit h = CrowPanelFileBrowser::hitTest(pts[0].x, pts[0].y);
    if (h == Hit::Cancel) {
      CrowPanelFileBrowser::close();
    } else if (h == Hit::Up) {
      CrowPanelFileBrowser::scrollBy(-1);
    } else if (h == Hit::Down) {
      CrowPanelFileBrowser::scrollBy(1);
    } else if (h == Hit::File) {
      int idx = CrowPanelFileBrowser::fileIndex();
      const char *nm = CrowPanelFileBrowser::nameAt(idx);
      if (nm && nm[0]) {
        ExtCmdQueue::ExternalCmd extcmd{};
        extcmd.cmd = ExtCmd::ATTACHD64ANDLOAD;
        memset(extcmd.param, 0, sizeof(extcmd.param));
        strncpy(reinterpret_cast<char *>(&extcmd.param[2]), nm,
                sizeof(extcmd.param) - 3);
        ExtCmdQueue::getInstance().push(extcmd);
      }
      CrowPanelFileBrowser::close();
    }
    CrowPanelHw::setHighlight(-1);
    return;
  }

  // Soft pad — check before matrix keys.
  // Prefer the first non-joy hit so keys work while holding a direction.
  {
    using CrowPanelSoftPad::Hit;
    Hit sh = Hit::None;
    for (int i = 0; i < n; i++) {
      Hit h = CrowPanelSoftPad::hitTest(pts[i].x, pts[i].y);
      if (h == Hit::None) {
        continue;
      }
      if (joyProfile && joyBitForSlot((int)h) >= 0) {
        continue; // joy already handled continuously
      }
      sh = h;
      break;
    }
    if (sh != Hit::None) {
      CrowPanelHw::setHighlight(-1);
      if (sh == Hit::Logo) {
        clearHold();
        CrowPanelSoftPad::setHighlight(-1);
        ExtCmdQueue::ExternalCmd grab{};
        grab.cmd = ExtCmd::SCREENGRAB;
        memset(grab.param, 0, sizeof(grab.param));
        ExtCmdQueue::getInstance().push(grab);
        return;
      }
      CrowPanelSoftPad::setHighlight((int)sh);
      const int sid = (int)sh;
      if (CrowPanelSoftPad::programMode()) {
        clearHold();
        CrowPanelSoftPad::onSelectSlot(sid);
      } else {
        const auto &s = CrowPanelSoftPad::slot(sid);
        if (s.assigned()) {
          startPulse(s.code());
          beginHold(true, sid, s.code());
        } else {
          clearHold();
        }
      }
      return;
    }
    if (joyActive) {
      return; // only joy contacts this frame
    }
  }

  int hit = crowHitTest(pts[0].x, pts[0].y);
  if (hit < 0) {
    return;
  }

  const CrowKeyDef &k = CrowKeys[hit];
  activeKey = hit;
  CrowPanelHw::setHighlight(hit);
  CrowPanelSoftPad::setHighlight(-1);

  // Programming: next matrix key binds to the selected pad slot.
  if (CrowPanelSoftPad::programMode() &&
      CrowPanelSoftPad::selectedSlot() >= 0 &&
      k.kind == CrowKeyKind::Matrix) {
    CrowPanelSoftPad::assignFromKey(crowKeyFaceLabel(k, false), k.code);
    CrowPanelHw::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtList) {
    CrowPanelFileBrowser::close();
    CrowPanelSetup::close();
    CrowPanelSoftPad::closeSelect();
    CrowPanelDirBrowser::open();
    ExtCmdQueue::ExternalCmd extcmd{};
    extcmd.cmd = ExtCmd::LISTD64DIR;
    memset(extcmd.param, 0, sizeof(extcmd.param));
    ExtCmdQueue::getInstance().push(extcmd);
    CrowPanelHw::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtAttach) {
    CrowPanelDirBrowser::close();
    CrowPanelSetup::close();
    CrowPanelSoftPad::closeSelect();
    CrowPanelFileBrowser::open();
    CrowPanelHw::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtLoad) {
    // LOAD"*",8 only — press RUN separately when ready.
    ExtCmdQueue::ExternalCmd loadCmd{};
    loadCmd.cmd = ExtCmd::LOAD;
    memset(loadCmd.param, 0, sizeof(loadCmd.param));
    ExtCmdQueue::getInstance().push(loadCmd);
    CrowPanelHw::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtSetup) {
    CrowPanelFileBrowser::close();
    CrowPanelDirBrowser::close();
    CrowPanelSoftPad::closeSelect();
    CrowPanelSetup::open();
    CrowPanelHw::setHighlight(-1);
    CrowPanelSoftPad::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtPad) {
    CrowPanelFileBrowser::close();
    CrowPanelDirBrowser::close();
    CrowPanelSetup::close();
    clearHold();
    if (CrowPanelSoftPad::selectActive()) {
      CrowPanelSoftPad::closeSelect();
    } else {
      CrowPanelSoftPad::openSelect();
    }
    CrowPanelHw::setHighlight(-1);
    CrowPanelSoftPad::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtProg) {
    CrowPanelSoftPad::closeSelect();
    clearHold();
    CrowPanelSoftPad::onProg();
    CrowPanelHw::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ModShift) {
    sticky ^= 1;
    applyModsOnly();
    return;
  }
  if (k.kind == CrowKeyKind::ModCtrl) {
    sticky ^= 2;
    applyModsOnly();
    return;
  }
  if (k.kind == CrowKeyKind::ModCbm) {
    sticky ^= 4;
    applyModsOnly();
    return;
  }
  if (k.kind == CrowKeyKind::ShiftLock) {
    shiftLock = !shiftLock;
    applyModsOnly();
    return;
  }
  if (k.kind == CrowKeyKind::None) {
    CrowPanelHw::setHighlight(-1);
    return;
  }

  ExtCmd ecmd = ExtCmd::NOEXTCMD;
  uint8_t eparam = 0;
  if (crowExtCmdForKind(k.kind, ecmd, eparam)) {
    ExtCmdQueue::ExternalCmd extcmd{};
    extcmd.cmd = ecmd;
    memset(extcmd.param, 0, sizeof(extcmd.param));
    extcmd.param[0] = eparam;
    ExtCmdQueue::getInstance().push(extcmd);
    CrowPanelHw::setHighlight(-1);
    return;
  }

  startPulse(k.code);
  beginHold(false, hit, k.code);
}

uint8_t CrowPanelTouchKB::getKBCodeDC01() {
  return sentdc01.load(std::memory_order_acquire);
}
uint8_t CrowPanelTouchKB::getKBCodeDC00() {
  return sentdc00.load(std::memory_order_acquire);
}
uint8_t CrowPanelTouchKB::getShiftctrlcode() {
  return shiftctrlcode.load(std::memory_order_acquire);
}
uint8_t CrowPanelTouchKB::getKBJoyValue() {
  return joyvalue.load(std::memory_order_acquire);
}

#endif
