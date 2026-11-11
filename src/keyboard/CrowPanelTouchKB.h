/*
 On-screen C64 keyboard for CrowPanel (touch → CIA matrix codes).
 Soft-pad / matrix keys: hold 0.5s to auto-repeat.
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
  // Active-low CIA joystick (bit0 up … bit4 fire). Soft-pad JOY profile.
  std::atomic<uint8_t> joyvalue{0xff};

  bool shiftLock = false;
  uint8_t sticky = 0; // bit0 shift, bit1 ctrl, bit2 cbm

  bool fingerDown = false;      // raw touch contact
  bool awaitingRelease = false; // ignore until finger lifts
  int releaseQuiet = 0;         // consecutive no-touch scans before re-arm
  int pulseLeft = 0;            // remaining scan cycles to assert matrix key
  int activeKey = -1;

  // Hold-to-repeat (soft pad + matrix keys).
  bool holdRepeat = false;
  bool holdIsPad = false;
  bool holdIsJoy = false; // soft-pad JOY D-pad/FIRE — hold joy bit, no matrix
  int holdId = -1; // pad slot or keyboard key index
  CodeTripleS holdCode{};
  uint32_t holdStartMs = 0;
  uint32_t lastFireMs = 0;

  static constexpr int kPulseCycles = 5; // ~40ms at 8ms scan rate
  // Touch controllers often drop a frame mid-press; require sustained lift
  // before the next tap can fire (else LOAD/etc. queues twice).
  static constexpr int kReleaseQuietScans = 10; // ~80ms at 8ms scan
  static constexpr uint32_t kHoldRepeatDelayMs = 500;
  static constexpr uint32_t kHoldRepeatRateMs = 80;

  void clearMatrix();
  void clearJoy();
  void applyModsOnly();
  void startPulse(const CodeTripleS &code);
  void clearHold();
  void beginHold(bool isPad, int id, const CodeTripleS &code);
  void pollHoldRepeat(int touchX, int touchY);
  static int joyBitForSlot(int slot); // 0..4 or -1
  // OR joy bits from all touch points on D-pad/FIRE (diagonals).
  // Returns true if any joy control is currently pressed.
  bool updateJoyFromPoints(const void *pts, int n);

public:
  void init() override;
  void scanKeyboard() override;
  uint8_t getKBCodeDC01() override;
  uint8_t getKBCodeDC00() override;
  uint8_t getShiftctrlcode() override;
  uint8_t getKBJoyValue() override;
};

#endif
#endif
