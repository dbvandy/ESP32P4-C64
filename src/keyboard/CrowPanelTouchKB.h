/*
 On-screen C64 keyboard for CrowPanel (touch → CIA matrix codes).
 One tap = one keypress (no hold-repeat / bounce).
*/
#ifndef CROWPANEL_TOUCH_KB_H
#define CROWPANEL_TOUCH_KB_H

#include "../Config.h"
#ifdef USE_CROWPANEL_TOUCH_KEYBOARD
#include "CodeTripleDef.h"
#include "KeyboardDriver.h"
#include <atomic>
#include <cstdint>

class CrowPanelTouchKB : public KeyboardDriver {
private:
  std::atomic<uint8_t> sentdc00{0xff};
  std::atomic<uint8_t> sentdc01{0xff};
  std::atomic<uint8_t> shiftctrlcode{0};

  bool shiftLock = false;
  uint8_t sticky = 0; // bit0 shift, bit1 ctrl, bit2 cbm

  bool fingerDown = false;     // raw touch contact
  bool awaitingRelease = false; // ignore until finger lifts
  int releaseQuiet = 0;        // consecutive no-touch scans before re-arm
  int pulseLeft = 0;           // remaining scan cycles to assert matrix key
  int activeKey = -1;

  static constexpr int kPulseCycles = 5; // ~40ms at 8ms scan rate
  // Touch controllers often drop a frame mid-press; require sustained lift
  // before the next tap can fire (else LOAD/etc. queues twice).
  static constexpr int kReleaseQuietScans = 10; // ~80ms at 8ms scan

  void clearMatrix();
  void applyModsOnly();
  void startPulse(const CodeTripleS &code);

public:
  void init() override;
  void scanKeyboard() override;
  uint8_t getKBCodeDC01() override;
  uint8_t getKBCodeDC00() override;
  uint8_t getShiftctrlcode() override;
};

#endif
#endif
