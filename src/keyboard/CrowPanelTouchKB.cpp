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
  holdId = -1;
  holdStartMs = 0;
  lastFireMs = 0;
}

void CrowPanelTouchKB::beginHold(bool isPad, int id, const CodeTripleS &code) {
  holdRepeat = true;
  holdIsPad = isPad;
  holdId = id;
  holdCode = code;
  holdStartMs = millis();
  lastFireMs = holdStartMs;
}

void CrowPanelTouchKB::pollHoldRepeat(int touchX, int touchY) {
  if (!holdRepeat || holdId < 0) {
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

  TouchPoint pts[1];
  int n = tp->readPoints(pts, 1, 0);

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

  // Still contacting after we already fired this press — maybe auto-repeat.
  if (awaitingRelease) {
    pollHoldRepeat(pts[0].x, pts[0].y);
    return;
  }

  // Rising edge only (finger was up, now down)
  if (fingerDown) {
    return;
  }
  fingerDown = true;
  awaitingRelease = true; // one event per contact

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

  // Soft pad (below keyboard) — check before matrix keys.
  {
    using CrowPanelSoftPad::Hit;
    Hit sh = CrowPanelSoftPad::hitTest(pts[0].x, pts[0].y);
    if (sh != Hit::None) {
      CrowPanelSoftPad::setHighlight((int)sh);
      CrowPanelHw::setHighlight(-1);
      if (CrowPanelSoftPad::isProfileHit((int)sh)) {
        clearHold();
        CrowPanelSoftPad::setProfile((int)sh - CrowPanelSoftPad::kProfileHitBase);
        return;
      }
      {
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
      }
      return;
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
    CrowPanelSetup::open();
    CrowPanelHw::setHighlight(-1);
    CrowPanelSoftPad::setHighlight(-1);
    return;
  }

  if (k.kind == CrowKeyKind::ExtProg) {
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

#endif
