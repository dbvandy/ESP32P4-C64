/*
 Commodore 64 physical keyboard + T-HMI-C64 control strip for CrowPanel.
 Portrait-logical: C64 keyboard on top, emulator controls below.
 Shift / Shift-Lock swaps legends to the shifted face characters.
*/
#ifndef CROWPANEL_KEY_LAYOUT_H
#define CROWPANEL_KEY_LAYOUT_H

#include "../Config.h"
#ifdef USE_CROWPANEL_DISPLAY
#include "../ExtCmd.h"
#include "../keyboard/C64Keycodes.h"
#include "CrowPanelHw.h"
#include <cstdint>

enum class CrowKeyKind : uint8_t {
  Matrix = 0, // normal CIA matrix key
  ModShift,   // sticky / hold left-shift
  ModCtrl,
  ModCbm,
  ShiftLock,  // toggle sticky shift
  ExtList,
  ExtLoad,
  ExtRun,
  ExtAttach,
  ExtDetach,
  ExtReset,
  ExtRestore,
  ExtSetup,
  ExtProg,
  ExtVolUp,
  ExtVolDn,
  ExtJoy1,
  ExtJoy2,
  ExtJoyOff,
  None
};

struct CrowKeyDef {
  int16_t col;            // grid column (0..)
  int16_t row;            // grid row (0..)
  int16_t colspan;        // width in grid units
  const char *label;      // unshifted legend
  const char *labelShift; // shifted legend (nullptr = same as label)
  CrowKeyKind kind;
  CodeTripleS code; // valid when kind == Matrix
};

inline const char *crowKeyFaceLabel(const CrowKeyDef &k, bool shifted) {
  if (shifted && k.labelShift && k.labelShift[0]) {
    return k.labelShift;
  }
  return k.label;
}

// Rows 0-4: C64 keyboard. Rows 5-6: T-HMI controls below.
inline constexpr int CrowKeyCols = 18;
inline constexpr int CrowKeyRows = 7;
inline constexpr int CrowKbRows = 5; // C64 key rows only

// labelShift follows classic C64 keycap legends (F1→F2, 1→!, :→[, etc.).
inline constexpr CrowKeyDef CrowKeys[] = {
    // Row 0
    {0, 0, 1, "<-", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_LEFTARROW},
    {1, 0, 1, "1", "!", CrowKeyKind::Matrix, C64_KEYCODE_1},
    {2, 0, 1, "2", "\"", CrowKeyKind::Matrix, C64_KEYCODE_2},
    {3, 0, 1, "3", "#", CrowKeyKind::Matrix, C64_KEYCODE_3},
    {4, 0, 1, "4", "$", CrowKeyKind::Matrix, C64_KEYCODE_4},
    {5, 0, 1, "5", "%", CrowKeyKind::Matrix, C64_KEYCODE_5},
    {6, 0, 1, "6", "&", CrowKeyKind::Matrix, C64_KEYCODE_6},
    {7, 0, 1, "7", "'", CrowKeyKind::Matrix, C64_KEYCODE_7},
    {8, 0, 1, "8", "(", CrowKeyKind::Matrix, C64_KEYCODE_8},
    {9, 0, 1, "9", ")", CrowKeyKind::Matrix, C64_KEYCODE_9},
    {10, 0, 1, "0", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_0},
    {11, 0, 1, "+", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_PLUS},
    {12, 0, 1, "-", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_MINUS},
    {13, 0, 1, "PO", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_POUND},
    {14, 0, 1, "CLR", "HOME", CrowKeyKind::Matrix, C64_KEYCODE_HOME},
    {15, 0, 1, "DEL", "INST", CrowKeyKind::Matrix, C64_KEYCODE_DEL},
    {16, 0, 2, "F1", "F2", CrowKeyKind::Matrix, C64_KEYCODE_F1},

    // Row 1
    {0, 1, 1, "CTL", nullptr, CrowKeyKind::ModCtrl, {0xff, 0xff, 0}},
    {1, 1, 1, "Q", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_Q},
    {2, 1, 1, "W", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_W},
    {3, 1, 1, "E", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_E},
    {4, 1, 1, "R", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_R},
    {5, 1, 1, "T", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_T},
    {6, 1, 1, "Y", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_Y},
    {7, 1, 1, "U", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_U},
    {8, 1, 1, "I", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_I},
    {9, 1, 1, "O", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_O},
    {10, 1, 1, "P", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_P},
    {11, 1, 1, "@", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_AT},
    {12, 1, 1, "*", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_ASTERISK},
    {13, 1, 1, "^", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_UPARROW},
    {14, 1, 2, "RSTR", nullptr, CrowKeyKind::ExtRestore, {0xff, 0xff, 0}},
    {16, 1, 2, "F3", "F4", CrowKeyKind::Matrix, C64_KEYCODE_F3},

    // Row 2
    {0, 2, 1, "R/S", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_BREAK},
    {1, 2, 1, "LCK", nullptr, CrowKeyKind::ShiftLock, {0xff, 0xff, 0}},
    {2, 2, 1, "A", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_A},
    {3, 2, 1, "S", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_S},
    {4, 2, 1, "D", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_D},
    {5, 2, 1, "F", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_F},
    {6, 2, 1, "G", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_G},
    {7, 2, 1, "H", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_H},
    {8, 2, 1, "J", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_J},
    {9, 2, 1, "K", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_K},
    {10, 2, 1, "L", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_L},
    {11, 2, 1, ":", "[", CrowKeyKind::Matrix, C64_KEYCODE_COLON},
    {12, 2, 1, ";", "]", CrowKeyKind::Matrix, C64_KEYCODE_SEMICOLON},
    {13, 2, 1, "=", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_EQUALS},
    {14, 2, 2, "RET", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_RETURN},
    {16, 2, 2, "F5", "F6", CrowKeyKind::Matrix, C64_KEYCODE_F5},

    // Row 3
    {0, 3, 1, "C=", nullptr, CrowKeyKind::ModCbm, {0xff, 0xff, 0}},
    {1, 3, 2, "SHIFT", nullptr, CrowKeyKind::ModShift, {0xff, 0xff, 0}},
    {3, 3, 1, "Z", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_Z},
    {4, 3, 1, "X", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_X},
    {5, 3, 1, "C", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_C},
    {6, 3, 1, "V", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_V},
    {7, 3, 1, "B", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_B},
    {8, 3, 1, "N", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_N},
    {9, 3, 1, "M", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_M},
    {10, 3, 1, ",", "<", CrowKeyKind::Matrix, C64_KEYCODE_COMMA},
    {11, 3, 1, ".", ">", CrowKeyKind::Matrix, C64_KEYCODE_PERIOD},
    {12, 3, 1, "/", "?", CrowKeyKind::Matrix, C64_KEYCODE_SLASH},
    {13, 3, 1, "SH", nullptr, CrowKeyKind::ModShift, {0xff, 0xff, 0}},
    {14, 3, 1, "DN", "UP", CrowKeyKind::Matrix, C64_KEYCODE_DOWN},
    {15, 3, 1, "RT", "LT", CrowKeyKind::Matrix, C64_KEYCODE_RIGHT},
    {16, 3, 2, "F7", "F8", CrowKeyKind::Matrix, C64_KEYCODE_F7},

    // Row 4 space bar (shortened so disk name fits beside it)
    {3, 4, 10, "SPACE", nullptr, CrowKeyKind::Matrix, C64_KEYCODE_SPACE},

    // ---- T-HMI-C64 controls below keyboard ----
    {0, 5, 3, "D64", nullptr, CrowKeyKind::ExtAttach, {0xff, 0xff, 0}},
    {3, 5, 3, "LOAD", nullptr, CrowKeyKind::ExtLoad, {0xff, 0xff, 0}},
    {6, 5, 3, "RUN", nullptr, CrowKeyKind::ExtRun, {0xff, 0xff, 0}},
    {9, 5, 3, "LIST", nullptr, CrowKeyKind::ExtList, {0xff, 0xff, 0}},
    {12, 5, 3, "EJT", nullptr, CrowKeyKind::ExtDetach, {0xff, 0xff, 0}},
    {15, 5, 3, "RST", nullptr, CrowKeyKind::ExtReset, {0xff, 0xff, 0}},

    {0, 6, 3, "PROG", nullptr, CrowKeyKind::ExtProg, {0xff, 0xff, 0}},
    {3, 6, 3, "", nullptr, CrowKeyKind::None, {0xff, 0xff, 0}},
    {6, 6, 3, "J1", nullptr, CrowKeyKind::ExtJoy1, {0xff, 0xff, 0}},
    {9, 6, 3, "J2", nullptr, CrowKeyKind::ExtJoy2, {0xff, 0xff, 0}},
    {12, 6, 3, "JOFF", nullptr, CrowKeyKind::ExtJoyOff, {0xff, 0xff, 0}},
    {15, 6, 3, "SETUP", nullptr, CrowKeyKind::ExtSetup, {0xff, 0xff, 0}},
};

inline constexpr int CrowKeyCount =
    (int)(sizeof(CrowKeys) / sizeof(CrowKeys[0]));

inline bool crowIsCtrlKind(CrowKeyKind k) {
  return k == CrowKeyKind::ExtList || k == CrowKeyKind::ExtLoad ||
         k == CrowKeyKind::ExtRun || k == CrowKeyKind::ExtAttach ||
         k == CrowKeyKind::ExtDetach || k == CrowKeyKind::ExtReset ||
         k == CrowKeyKind::ExtRestore || k == CrowKeyKind::ExtSetup ||
         k == CrowKeyKind::ExtProg || k == CrowKeyKind::ExtVolUp ||
         k == CrowKeyKind::ExtVolDn || k == CrowKeyKind::ExtJoy1 ||
         k == CrowKeyKind::ExtJoy2 || k == CrowKeyKind::ExtJoyOff;
}

inline bool crowExtCmdForKind(CrowKeyKind kind, ExtCmd &cmd, uint8_t &param0) {
  param0 = 0;
  switch (kind) {
  case CrowKeyKind::ExtList:
    // CrowPanelTouchKB opens the D64 directory window (LISTD64DIR).
    return false;
  case CrowKeyKind::ExtLoad:
    // CrowPanelTouchKB queues LOAD"*",8 only.
    return false;
  case CrowKeyKind::ExtRun:
    cmd = ExtCmd::BASICRUN;
    return true;
  case CrowKeyKind::ExtAttach:
    // CrowPanelTouchKB opens the .d64 picker (ATTACHD64ANDLOAD).
    return false;
  case CrowKeyKind::ExtDetach:
    cmd = ExtCmd::DETACHD64;
    return true;
  case CrowKeyKind::ExtReset:
    cmd = ExtCmd::RESET;
    return true;
  case CrowKeyKind::ExtRestore:
    cmd = ExtCmd::RESTORE;
    param0 = 0;
    return true;
  case CrowKeyKind::ExtSetup:
    // CrowPanelTouchKB opens the SETUP overlay.
    return false;
  case CrowKeyKind::ExtProg:
    // CrowPanelTouchKB toggles soft-pad program mode.
    return false;
  case CrowKeyKind::ExtVolUp:
    cmd = ExtCmd::INCVOLUME;
    param0 = 5;
    return true;
  case CrowKeyKind::ExtVolDn:
    cmd = ExtCmd::DECVOLUME;
    param0 = 5;
    return true;
  case CrowKeyKind::ExtJoy1:
    cmd = ExtCmd::KBJOYSTICKMODE1;
    return true;
  case CrowKeyKind::ExtJoy2:
    cmd = ExtCmd::KBJOYSTICKMODE2;
    return true;
  case CrowKeyKind::ExtJoyOff:
    cmd = ExtCmd::KBJOYSTICKMODEOFF;
    return true;
  default:
    return false;
  }
}

// Square unit cells for the full 7-row grid (KB + controls).
inline int crowKeyCell() {
  const int byW = CrowPanelHw::KB_PORTRAIT_W / CrowKeyCols;
  const int byH = CrowPanelHw::KB_PORTRAIT_H / CrowKeyRows;
  return byW < byH ? byW : byH;
}
inline int crowKeyGridW() { return crowKeyCell() * CrowKeyCols; }
inline int crowKeyGridH() { return crowKeyCell() * CrowKeyRows; }
inline int crowKeyOriginX() {
  return (CrowPanelHw::KB_PORTRAIT_W - crowKeyGridW()) / 2;
}
// Pin grid toward the top so controls sit clearly under the keyboard.
inline int crowKeyOriginY() {
  const int pad = 8;
  return pad;
}

inline void crowKeyRectPortrait(const CrowKeyDef &k, int &px, int &py, int &pw,
                                int &ph) {
  const int pad = 2;
  const int cell = crowKeyCell();
  px = crowKeyOriginX() + k.col * cell + pad;
  py = crowKeyOriginY() + k.row * cell + pad;
  pw = k.colspan * cell - pad * 2;
  ph = cell - pad * 2;
}

inline void crowKeyRect(const CrowKeyDef &k, int &x, int &y, int &w, int &h) {
  int px, py, pw, ph;
  crowKeyRectPortrait(k, px, py, pw, ph);
  x = (int)CrowPanelHw::KB_X0 + py;
  y = (int)CrowPanelHw::PANEL_H - (px + pw);
  w = ph;
  h = pw;
}

inline int crowHitTest(int tx, int ty) {
  if (tx < (int)CrowPanelHw::KB_X0) {
    return -1;
  }
  const int px = (int)CrowPanelHw::PANEL_H - 1 - ty;
  const int py = tx - (int)CrowPanelHw::KB_X0;
  if (px < 0 || py < 0 || px >= (int)CrowPanelHw::KB_PORTRAIT_W ||
      py >= (int)CrowPanelHw::KB_PORTRAIT_H) {
    return -1;
  }
  for (int i = 0; i < CrowKeyCount; i++) {
    int kpx, kpy, kpw, kph;
    crowKeyRectPortrait(CrowKeys[i], kpx, kpy, kpw, kph);
    if (px >= kpx && px < kpx + kpw && py >= kpy && py < kpy + kph) {
      return i;
    }
  }
  return -1;
}

#endif
#endif
