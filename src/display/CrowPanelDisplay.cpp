/*
 CrowPanel 10.1" MIPI-DSI display driver for T-HMI-C64.
*/
#include "../Config.h"
#ifdef USE_CROWPANEL_DISPLAY

#include "CrowPanelDisplay.h"
#include "C64LogoBitmap.h"
#include "CrowPanelDirBrowser.h"
#include "CrowPanelFileBrowser.h"
#include "CrowPanelHw.h"
#include "CrowPanelKeyLayout.h"
#include "CrowPanelSetup.h"
#include "CrowPanelSoftPad.h"
#include "../roms/charset.h"
#include "board_config.h"
#include "esp_panel_board_custom_conf.h"
#include "esp_panel_drivers_conf.h"
#include "esp_utils_conf.h"
#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>

using namespace esp_panel::board;
using namespace esp_panel::drivers;

namespace CrowPanelHw {
static Board *gBoard = nullptr;
static LCD *gLcd = nullptr;
static Touch *gTouch = nullptr;
static uint16_t *gFb = nullptr;
static std::atomic<bool> gKbRedraw{true};
static uint8_t gStickyMods = 0;
static int gHighlight = -1;
static char gAttachedDisk[17] = "";
static uint8_t gDriveLedHold = 0;

Board *board() { return gBoard; }
LCD *lcd() { return gLcd; }
Touch *touch() { return gTouch; }
uint16_t *framebuffer() { return gFb; }

void present() {
  if (!gLcd || !gFb) {
    return;
  }
  gLcd->drawBitmap(0, 0, PANEL_W, PANEL_H,
                   reinterpret_cast<const uint8_t *>(gFb), 40);
}

void requestKeyboardRedraw() {
  gKbRedraw.store(true, std::memory_order_release);
}
bool keyboardNeedsRedraw() {
  return gKbRedraw.load(std::memory_order_acquire);
}
void clearKeyboardRedraw() {
  gKbRedraw.store(false, std::memory_order_release);
}

void setStickyMods(uint8_t m) {
  if (gStickyMods == m) {
    return;
  }
  gStickyMods = m;
  requestKeyboardRedraw();
}
uint8_t stickyMods() { return gStickyMods; }
void setHighlight(int idx) {
  if (gHighlight != idx) {
    gHighlight = idx;
    requestKeyboardRedraw();
  }
}
int highlight() { return gHighlight; }

void setAttachedDisk(const char *name) {
  char buf[sizeof(gAttachedDisk)];
  buf[0] = '\0';
  if (name && name[0]) {
    const char *base = name;
    for (const char *p = name; *p; ++p) {
      if (*p == '/' || *p == '\\') {
        base = p + 1;
      }
    }
    size_t n = 0;
    while (base[n] && n + 1 < sizeof(buf)) {
      char c = base[n];
      if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
      }
      buf[n++] = c;
    }
    buf[n] = '\0';
    if (n >= 4 && (strcmp(buf + n - 4, ".D64") == 0)) {
      buf[n - 4] = '\0';
    }
  }
  if (strncmp(gAttachedDisk, buf, sizeof(gAttachedDisk)) == 0) {
    return;
  }
  strncpy(gAttachedDisk, buf, sizeof(gAttachedDisk) - 1);
  gAttachedDisk[sizeof(gAttachedDisk) - 1] = '\0';
  requestKeyboardRedraw();
}
const char *attachedDisk() { return gAttachedDisk; }

void pulseDriveActivity() {
  const bool wasOn = gDriveLedHold > 0;
  gDriveLedHold = 12; // stay lit across several frames
  if (!wasOn) {
    requestKeyboardRedraw();
  }
}

void tickDriveActivity() {
  if (gDriveLedHold == 0) {
    return;
  }
  gDriveLedHold--;
  if (gDriveLedHold == 0) {
    requestKeyboardRedraw();
  }
}

bool driveReading() { return gDriveLedHold > 0; }

void setBoard(Board *b) { gBoard = b; }
void setLcd(LCD *l) { gLcd = l; }
void setTouch(Touch *t) { gTouch = t; }
void setFb(uint16_t *f) { gFb = f; }
} // namespace CrowPanelHw

static void fillRect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) {
    return;
  }
  for (int yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= (int)CrowPanelHw::PANEL_H) {
      continue;
    }
    uint16_t *row = fb + yy * CrowPanelHw::PANEL_W;
    for (int xx = x; xx < x + w; xx++) {
      if (xx >= 0 && xx < (int)CrowPanelHw::PANEL_W) {
        row[xx] = c;
      }
    }
  }
}

static void fillCircle(uint16_t *fb, int cx, int cy, int r, uint16_t c) {
  if (r <= 0) {
    return;
  }
  const int r2 = r * r;
  for (int yy = -r; yy <= r; yy++) {
    for (int xx = -r; xx <= r; xx++) {
      if (xx * xx + yy * yy > r2) {
        continue;
      }
      int px = cx + xx;
      int py = cy + yy;
      if (px >= 0 && px < (int)CrowPanelHw::PANEL_W && py >= 0 &&
          py < (int)CrowPanelHw::PANEL_H) {
        fb[py * CrowPanelHw::PANEL_W + px] = c;
      }
    }
  }
}

static void fillRoundRect(uint16_t *fb, int bx, int by, int bw, int bh, int rr,
                          uint16_t fill, uint16_t edge) {
  if (bw <= 0 || bh <= 0) {
    return;
  }
  if (rr * 2 > bw) {
    rr = bw / 2;
  }
  if (rr * 2 > bh) {
    rr = bh / 2;
  }
  if (rr < 1) {
    rr = 1;
  }
  fillRect(fb, bx + rr, by, bw - 2 * rr, bh, fill);
  fillRect(fb, bx, by + rr, bw, bh - 2 * rr, fill);
  fillCircle(fb, bx + rr, by + rr, rr, fill);
  fillCircle(fb, bx + bw - 1 - rr, by + rr, rr, fill);
  fillCircle(fb, bx + rr, by + bh - 1 - rr, rr, fill);
  fillCircle(fb, bx + bw - 1 - rr, by + bh - 1 - rr, rr, fill);
  fillRect(fb, bx + rr, by, bw - 2 * rr, 1, edge);
  fillRect(fb, bx + rr, by + bh - 1, bw - 2 * rr, 1, edge);
  fillRect(fb, bx, by + rr, 1, bh - 2 * rr, edge);
  fillRect(fb, bx + bw - 1, by + rr, 1, bh - 2 * rr, edge);
}

static void drawChar8Rot90CCW(uint16_t *fb, int cx, int cy, uint8_t ch,
                              uint16_t fg, uint16_t bg, int scale) {
  // Draw an 8x8 glyph rotated 90° CCW, centered at (cx, cy).
  uint8_t screen = 32;
  if (ch >= 'a' && ch <= 'z') {
    screen = (uint8_t)(ch - 'a' + 1);
  } else if (ch >= 'A' && ch <= 'Z') {
    screen = (uint8_t)(ch - 'A' + 1);
  } else if (ch >= '0' && ch <= '9') {
    screen = (uint8_t)(ch - '0' + 48);
  } else if (ch == '+') {
    screen = 43;
  } else if (ch == '-') {
    screen = 45;
  } else if (ch == '*') {
    screen = 42;
  } else if (ch == '/') {
    screen = 47;
  } else if (ch == '=') {
    screen = 61;
  } else if (ch == ':') {
    screen = 58;
  } else if (ch == ';') {
    screen = 59;
  } else if (ch == ',') {
    screen = 44;
  } else if (ch == '.') {
    screen = 46;
  } else if (ch == '@') {
    screen = 0;
  } else if (ch == '^') {
    screen = 30;
  } else if (ch == '<') {
    screen = 60;
  } else if (ch == '>') {
    screen = 62;
  } else if (ch == '!') {
    screen = 33;
  } else if (ch == '"') {
    screen = 34;
  } else if (ch == '#') {
    screen = 35;
  } else if (ch == '$') {
    screen = 36;
  } else if (ch == '%') {
    screen = 37;
  } else if (ch == '&') {
    screen = 38;
  } else if (ch == '\'') {
    screen = 39;
  } else if (ch == '(') {
    screen = 40;
  } else if (ch == ')') {
    screen = 41;
  } else if (ch == '[') {
    screen = 27;
  } else if (ch == ']') {
    screen = 29;
  } else if (ch == '?') {
    screen = 63;
  }
  const uint8_t *glyph = &charset_rom[screen * 8];
  // Unrotated glyph local: col right, row down. After 90° CCW:
  // (col,row) -> (row, 7-col). Center the 8*scale box on (cx,cy).
  const int box = 8 * scale;
  const int ox = cx - box / 2;
  const int oy = cy - box / 2;
  for (int row = 0; row < 8; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
      int lx = row;       // after CCW
      int ly = 7 - col;   // after CCW
      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
          int px = ox + lx * scale + dx;
          int py = oy + ly * scale + dy;
          if (px >= 0 && px < (int)CrowPanelHw::PANEL_W && py >= 0 &&
              py < (int)CrowPanelHw::PANEL_H) {
            fb[py * CrowPanelHw::PANEL_W + px] = color;
          }
        }
      }
    }
  }
}

void CrowPanelDisplay::drawKeyGlyph(int x, int y, const char *label,
                                    uint16_t fg, uint16_t bg, int scale) {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb || !label) {
    return;
  }
  if (scale < 1) {
    scale = 1;
  }
  // Labels are rotated 90° CCW. Portrait left→right == landscape −Y, so
  // advance upward in the framebuffer for upright reading.
  int len = (int)strlen(label);
  int advance = 8 * scale + 1;
  int total = len * advance;
  int startY = y + total / 2;
  for (int i = 0; i < len; i++) {
    drawChar8Rot90CCW(fb, x, startY - i * advance - 4 * scale,
                      (uint8_t)label[i], fg, bg, scale);
  }
}

void CrowPanelDisplay::drawPortraitButton(int px, int py, int pw, int ph,
                                          const char *label, uint16_t face,
                                          uint16_t ink) {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  int x, y, w, h;
  CrowPanelFileBrowser::portraitRectToFb(px, py, pw, ph, x, y, w, h);
  fillRect(fb, x, y, w, h, face);
  fillRect(fb, x, y, w, 1, ink);
  fillRect(fb, x, y + h - 1, w, 1, ink);
  fillRect(fb, x, y, 1, h, ink);
  fillRect(fb, x + w - 1, y, 1, h, ink);
  drawKeyGlyph(x + w / 2, y + h / 2, label, ink, face);
}

void CrowPanelDisplay::drawFileBrowserOverlay() {
  using namespace CrowPanelFileBrowser;
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  // Keep picker muted so it does not wash out the panel.
  const uint16_t ink = 0xC618;
  const uint16_t face = 0x2945;
  const uint16_t faceCtrl = 0x3186;
  const uint16_t rowFace = 0x2104;
  const uint16_t titleFace = 0x3186;
  const int pw = CrowPanelHw::KB_PORTRAIT_W;
  const int ph = CrowPanelHw::KB_PORTRAIT_H;

  drawPortraitButton(kPad, 4, pw - 2 * kPad, kHeaderH - 8, "D64 DISKS",
                     titleFace, ink);

  int n = count();
  int sc = scroll();
  if (n == 0) {
    drawPortraitButton(kPad, kHeaderH + 8, pw - 2 * kPad, kRowH, "NO DISKS",
                       rowFace, ink);
  } else {
    for (int row = 0; row < kVisibleRows; row++) {
      int idx = sc + row;
      if (idx >= n) {
        break;
      }
      const char *nm = nameAt(idx);
      char label[20];
      memset(label, 0, sizeof(label));
      if (nm) {
        size_t len = strlen(nm);
        size_t copy = len;
        if (copy >= 4 && nm[copy - 4] == '.' &&
            (nm[copy - 3] == 'd' || nm[copy - 3] == 'D') &&
            nm[copy - 2] == '6' &&
            (nm[copy - 1] == '4')) {
          copy -= 4;
        }
        if (copy > sizeof(label) - 1) {
          copy = sizeof(label) - 1;
        }
        memcpy(label, nm, copy);
        for (char *p = label; *p; ++p) {
          if (*p >= 'a' && *p <= 'z') {
            *p = (char)(*p - 32);
          }
        }
      }
      int rowY = kHeaderH + 4 + row * kRowH;
      drawPortraitButton(kPad, rowY, pw - 2 * kPad, kRowH - 4, label, rowFace,
                         ink);
    }
  }

  int footerY = ph - kFooterH + 4;
  int third = pw / 3;
  drawPortraitButton(4, footerY, third - 8, kFooterH - 8, "X", faceCtrl, ink);
  drawPortraitButton(third + 4, footerY, third - 8, kFooterH - 8, "UP", face,
                     ink);
  drawPortraitButton(2 * third + 4, footerY, third - 8, kFooterH - 8, "DN", face,
                     ink);
}

void CrowPanelDisplay::drawDirBrowserOverlay() {
  using namespace CrowPanelDirBrowser;
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  const uint16_t ink = 0xC618;
  const uint16_t face = 0x2945;
  const uint16_t faceCtrl = 0x3186;
  const uint16_t rowFace = 0x2104;
  const uint16_t titleFace = 0x3186;
  const uint16_t scrollBg = 0x18C3;
  const uint16_t scrollFg = 0x7BEF;
  const int pw = CrowPanelHw::KB_PORTRAIT_W;
  const int ph = CrowPanelHw::KB_PORTRAIT_H;
  const int listW = pw - 2 * kPad - kScrollW - 4;

  const char *ttl = title();
  drawPortraitButton(kPad, 4, pw - 2 * kPad, kHeaderH - 8,
                     (ttl && ttl[0]) ? ttl : "DIRECTORY", titleFace, ink);

  int n = count();
  int sc = scroll();
  const char *st = status();
  if (st && st[0] && n == 0) {
    drawPortraitButton(kPad, kHeaderH + 8, listW, kRowH, st, rowFace, ink);
  } else if (n == 0) {
    drawPortraitButton(kPad, kHeaderH + 8, listW, kRowH, "EMPTY", rowFace, ink);
  } else {
    for (int row = 0; row < kVisibleRows; row++) {
      int idx = sc + row;
      if (idx >= n) {
        break;
      }
      const char *nm = nameAt(idx);
      const char *ty = typeAt(idx);
      char label[24];
      memset(label, 0, sizeof(label));
      if (nm) {
        snprintf(label, sizeof(label), "%s %s", nm, ty ? ty : "");
      }
      int rowY = kHeaderH + 4 + row * kRowH;
      drawPortraitButton(kPad, rowY, listW, kRowH - 4, label, rowFace, ink);
    }
  }

  // Scrollbar track + thumb
  {
    int trackX = pw - kPad - kScrollW;
    int trackY = kHeaderH + 4;
    int trackH = ph - kFooterH - kHeaderH - 8;
    drawPortraitButton(trackX, trackY, kScrollW, trackH, "", scrollBg, ink);
    if (n > kVisibleRows && trackH > 8) {
      int maxScroll = n - kVisibleRows;
      int thumbH = trackH * kVisibleRows / n;
      if (thumbH < 16) {
        thumbH = 16;
      }
      int thumbY =
          trackY + (maxScroll > 0 ? (sc * (trackH - thumbH)) / maxScroll : 0);
      drawPortraitButton(trackX + 2, thumbY, kScrollW - 4, thumbH, "", scrollFg,
                         ink);
    }
  }

  int footerY = ph - kFooterH + 4;
  int third = pw / 3;
  drawPortraitButton(4, footerY, third - 8, kFooterH - 8, "CLOSE", faceCtrl,
                     ink);
  drawPortraitButton(third + 4, footerY, third - 8, kFooterH - 8, "UP", face,
                     ink);
  drawPortraitButton(2 * third + 4, footerY, third - 8, kFooterH - 8, "DN", face,
                     ink);
}

void CrowPanelDisplay::drawSetupOverlay() {
  using namespace CrowPanelSetup;
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  // iPhone sheet in portrait key-space → 90° CCW on the landscape strip.
  const uint16_t ink = 0x1082;
  const uint16_t card = 0xF79E;     // #F2F2F7
  const uint16_t cardEdge = 0xC618;
  const uint16_t rowBg = 0xFFFF;
  const uint16_t trackEmpty = 0xE71C;
  const uint16_t fillB = 0x3D7F;
  const uint16_t fillV = 0xFD20;
  const uint16_t knob = 0xFFFF;
  const uint16_t knobEdge = 0x8410;
  const uint16_t closeBg = 0xCE79;

  auto p2fb = [](int px, int py, int pw, int ph, int &x, int &y, int &w,
                 int &h) {
    CrowPanelSoftPad::portraitToFb(px, py, x, y);
    int x1, y1;
    CrowPanelSoftPad::portraitToFb(px + pw - 1, py + ph - 1, x1, y1);
    int bx = x < x1 ? x : x1;
    int by = y < y1 ? y : y1;
    w = (x > x1 ? x : x1) - bx + 1;
    h = (y > y1 ? y : y1) - by + 1;
    x = bx;
    y = by;
  };

  auto fillRoundRectFb = [&](int bx, int by, int bw, int bh, int rr,
                             uint16_t fill, uint16_t edge) {
    if (rr * 2 > bw) {
      rr = bw / 2;
    }
    if (rr * 2 > bh) {
      rr = bh / 2;
    }
    if (rr < 2) {
      rr = 2;
    }
    fillRect(fb, bx + rr, by, bw - 2 * rr, bh, fill);
    fillRect(fb, bx, by + rr, bw, bh - 2 * rr, fill);
    fillCircle(fb, bx + rr, by + rr, rr, fill);
    fillCircle(fb, bx + bw - 1 - rr, by + rr, rr, fill);
    fillCircle(fb, bx + rr, by + bh - 1 - rr, rr, fill);
    fillCircle(fb, bx + bw - 1 - rr, by + bh - 1 - rr, rr, fill);
    fillRect(fb, bx + rr, by, bw - 2 * rr, 1, edge);
    fillRect(fb, bx + rr, by + bh - 1, bw - 2 * rr, 1, edge);
    fillRect(fb, bx, by + rr, 1, bh - 2 * rr, edge);
    fillRect(fb, bx + bw - 1, by + rr, 1, bh - 2 * rr, edge);
  };

  int px, py, pw, ph;
  panelPortrait(px, py, pw, ph);
  int x, y, w, h;
  p2fb(px, py, pw, ph, x, y, w, h);

  fillRoundRectFb(x + 3, y + 4, w, h, 18, 0x4208, 0x4208);
  fillRoundRectFb(x, y, w, h, 18, card, cardEdge);

  // Portrait-top maps to the left edge of the FB card after 90° CCW.
  drawKeyGlyph(x + 22, y + h / 2, "Setup", ink, card, 2);

  const int inset = 12;
  fillRoundRectFb(x + 44, y + inset, w - 44 - inset, h - 2 * inset, 12, rowBg,
                  cardEdge);

  auto drawSlider = [&](void (*trackFn)(int &, int &, int &, int &),
                        uint8_t value, uint8_t maxv, uint16_t fill,
                        const char *title) {
    int tx, ty, tw, th;
    trackFn(tx, ty, tw, th);
    int sx, sy, sw, sh;
    p2fb(tx, ty, tw, th, sx, sy, sw, sh);

    // Label above the slider in portrait space (smaller py than the track).
    int lpx = tx + tw / 2;
    int lpy = ty - 14;
    if (lpy < py + 4) {
      lpy = py + 4;
    }
    int lx, ly;
    CrowPanelSoftPad::portraitToFb(lpx, lpy, lx, ly);
    drawKeyGlyph(lx, ly, title, ink, rowBg, 1);

    // Portrait-horizontal track → slim vertical pill on screen (90° CCW).
    const int rr = sw / 2;
    fillRoundRectFb(sx, sy, sw, sh, rr < 2 ? 2 : rr, trackEmpty, trackEmpty);
    int filled = (int)((uint32_t)value * (uint32_t)sh / (uint32_t)maxv);
    if (filled < sw) {
      filled = (value > 0) ? sw : 0;
    }
    if (filled > 0) {
      // Portrait +X maps to screen −Y, so fill grows upward from bottom.
      int fy0 = sy + sh - filled;
      fillRoundRectFb(sx, fy0, sw, filled, rr < 2 ? 2 : rr, fill, fill);
    }
    int ky = sy + sh - filled;
    if (ky < sy) {
      ky = sy;
    }
    if (ky > sy + sh - 1) {
      ky = sy + sh - 1;
    }
    const int kr = sw / 2 + 5;
    fillCircle(fb, sx + sw / 2, ky, kr, knobEdge);
    fillCircle(fb, sx + sw / 2, ky, kr - 1, knob);
  };

  drawSlider(brightTrackPortrait, brightness(), 100, fillB, "Bright");
  drawSlider(volTrackPortrait, volume(), 255, fillV, "Volume");

  {
    int qx, qy, qw, qh, cx, cy, cw, ch;
    closeBtnPortrait(qx, qy, qw, qh);
    p2fb(qx, qy, qw, qh, cx, cy, cw, ch);
    int ccx = cx + cw / 2;
    int ccy = cy + ch / 2;
    int cr = (cw < ch ? cw : ch) / 2;
    fillCircle(fb, ccx, ccy, cr, closeBg);
    drawKeyGlyph(ccx, ccy, "X", ink, closeBg, 1);
  }
}

void CrowPanelDisplay::drawKeyboardOverlay() {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  const uint16_t bg = 0x3186;
  // Classic brown C64-style keys with white legends.
  const uint16_t face = 0x62C8;    // warm brown
  const uint16_t faceHi = 0x8C2A;  // lighter brown (pressed)
  const uint16_t faceMod = 0x49E4; // darker brown (mods idle)
  const uint16_t faceOn = 0xB4E6;  // highlight brown (mods on)
  const uint16_t faceCtrl = 0x51A5; // control-strip brown
  const uint16_t ink = 0xFFFF;     // white characters
  const uint16_t keyEdge = 0x3143;

  fillRect(fb, CrowPanelHw::KB_X0, 0, CrowPanelHw::KB_W, CrowPanelHw::KB_H, bg);

  if (CrowPanelDirBrowser::active()) {
    drawDirBrowserOverlay();
    CrowPanelHw::clearKeyboardRedraw();
    return;
  }

  if (CrowPanelFileBrowser::active()) {
    drawFileBrowserOverlay();
    CrowPanelHw::clearKeyboardRedraw();
    return;
  }

  uint8_t mods = CrowPanelHw::stickyMods();
  const bool shifted = (mods & 1) != 0;
  int hi = CrowPanelHw::highlight();

  for (int i = 0; i < CrowKeyCount; i++) {
    const CrowKeyDef &k = CrowKeys[i];
    int x, y, w, h;
    crowKeyRect(k, x, y, w, h);
    uint16_t col = face;
    if (k.kind == CrowKeyKind::ExtProg && CrowPanelSoftPad::programMode()) {
      col = faceOn;
    } else if (crowIsCtrlKind(k.kind)) {
      col = faceCtrl;
    } else if (k.kind == CrowKeyKind::ModShift ||
               k.kind == CrowKeyKind::ShiftLock) {
      col = shifted ? faceOn : faceMod;
    } else if (k.kind == CrowKeyKind::ModCtrl) {
      col = (mods & 2) ? faceOn : faceMod;
    } else if (k.kind == CrowKeyKind::ModCbm) {
      col = (mods & 4) ? faceOn : faceMod;
    }
    if (i == hi) {
      col = faceHi;
    }
    const int rr = (w < h ? w : h) / 3;
    fillRoundRect(fb, x, y, w, h, rr < 5 ? 5 : rr, col, keyEdge);
    const char *lab = crowKeyFaceLabel(k, shifted);
    if (k.kind == CrowKeyKind::ExtProg && CrowPanelSoftPad::programMode()) {
      lab = "SET";
    }
    if (lab && lab[0]) {
      drawKeyGlyph(x + w / 2, y + h / 2, lab, ink, col);
    }
  }

  // Disk name label to the right of SPACE (cols 13-17, row 4).
  {
    CrowKeyDef diskSlot{13, 4, 5, "", nullptr, CrowKeyKind::None, {0xff, 0xff, 0}};
    int x, y, w, h;
    crowKeyRect(diskSlot, x, y, w, h);
    const char *disk = CrowPanelHw::attachedDisk();
    const uint16_t diskFace = disk && disk[0] ? faceOn : faceCtrl;
    const int rr = (w < h ? w : h) / 3;
    fillRoundRect(fb, x, y, w, h, rr < 5 ? 5 : rr, diskFace, keyEdge);
    drawKeyGlyph(x + w / 2, y + h / 2, (disk && disk[0]) ? disk : "----", ink,
                 diskFace);
    // Drive activity LED (red) while sectors are being read.
    if (CrowPanelHw::driveReading()) {
      const uint16_t led = 0xF800; // bright red
      fillCircle(fb, x + w / 2, y + 8, 5, led);
    }
  }

  // Arcade soft pad: beige panel on grey margin, round lit buttons.
  {
    using namespace CrowPanelSoftPad;
    const uint16_t marginGrey = 0x3186; // same as KB bg
    const uint16_t beige = 0xE6D5;      // controller face
    const uint16_t beigeEdge = 0xC490;  // warm outline
    const uint16_t blue = 0x049F;
    const uint16_t blueLit = 0x7DFF;
    const uint16_t red = 0xE8C4;
    const uint16_t redLit = 0xFBAE;
    const uint16_t bezel = 0x1082;
    const uint16_t spec = 0xFFFF;
    const int shi = highlight();
    const int margin = 6; // grey ring around beige panel

    auto fillRoundRectFb = [&](int bx, int by, int bw, int bh, int rr,
                               uint16_t fill, uint16_t edge) {
      if (rr * 2 > bw) {
        rr = bw / 2;
      }
      if (rr * 2 > bh) {
        rr = bh / 2;
      }
      if (rr < 4) {
        rr = 4;
      }
      fillRect(fb, bx + rr, by, bw - 2 * rr, bh, fill);
      fillRect(fb, bx, by + rr, bw, bh - 2 * rr, fill);
      fillCircle(fb, bx + rr, by + rr, rr, fill);
      fillCircle(fb, bx + bw - 1 - rr, by + rr, rr, fill);
      fillCircle(fb, bx + rr, by + bh - 1 - rr, rr, fill);
      fillCircle(fb, bx + bw - 1 - rr, by + bh - 1 - rr, rr, fill);
      // Thin edge
      fillRect(fb, bx + rr, by, bw - 2 * rr, 2, edge);
      fillRect(fb, bx + rr, by + bh - 2, bw - 2 * rr, 2, edge);
      fillRect(fb, bx, by + rr, 2, bh - 2 * rr, edge);
      fillRect(fb, bx + bw - 2, by + rr, 2, bh - 2 * rr, edge);
    };

    int bx, by, bw, bh;
    bodyPortrait(bx, by, bw, bh);
    if (bw > 0 && bh > 0) {
      int x0, y0, x1, y1;
      portraitToFb(bx, by, x0, y0);
      portraitToFb(bx + bw - 1, by + bh - 1, x1, y1);
      int fx = x0 < x1 ? x0 : x1;
      int fy = y0 < y1 ? y0 : y1;
      int fw = (x0 > x1 ? x0 : x1) - fx + 1;
      int fh = (y0 > y1 ? y0 : y1) - fy + 1;
      // Grey margin (already KB bg, stamp explicitly).
      fillRect(fb, fx, fy, fw, fh, marginGrey);
      // Beige rounded panel inset a few pixels — modest corner radius (not
      // Nintendo capsule ends).
      const int inset = margin;
      int rr = 14;
      fillRoundRectFb(fx + inset, fy + inset, fw - 2 * inset, fh - 2 * inset, rr,
                      beige, beigeEdge);
    }

    auto drawArcadeBtn = [&](int slotIdx, bool redBtn) {
      int pcx, pcy, pr;
      buttonPortrait(slotIdx, pcx, pcy, pr);
      if (pr <= 0) {
        return;
      }
      int cx, cy;
      portraitToFb(pcx, pcy, cx, cy);
      const bool pressed = (shi == slotIdx);
      const bool selected = programMode() && selectedSlot() == slotIdx;
      const uint16_t face = redBtn ? red : blue;
      const uint16_t glow = redBtn ? redLit : blueLit;
      const uint16_t chrome = 0xC618; // metal ring
      const uint16_t chromeDk = 0x7BEF;

      // Always-on ring (same size as lit ring), grey when idle.
      const uint16_t ringOuter = 0x7BEF; // mid grey
      const uint16_t ringInner = 0x8410; // darker grey
      fillCircle(fb, cx, cy, pr + 8, ringOuter);
      fillCircle(fb, cx, cy, pr + 4, ringInner);
      // Lit: same ring, glow + white fill — 0.25s timed.
      if (pressed || selected) {
        fillCircle(fb, cx, cy, pr + 8, glow);
        fillCircle(fb, cx, cy, pr + 4, 0xFFFF);
      }
      // Real arcade stack: outer metal ring → black well → colored dome.
      fillCircle(fb, cx, cy, pr, chrome);
      fillCircle(fb, cx, cy, pr - 2, chromeDk);
      fillCircle(fb, cx, cy, pr - 4, bezel);
      fillCircle(fb, cx, cy, pr - 7, selected ? faceOn : face);
      // Specular highlight
      fillCircle(fb, cx - pr / 3, cy - pr / 3, pr / 5, spec);
      const uint16_t inner = selected ? faceOn : face;
      fillCircle(fb, cx, cy, pr / 2, inner);
      const char *lab = slot(slotIdx).label;
      // Bigger face letter (2×). Red buttons use white text for contrast.
      // Blank/unlabeled slots stay empty (no "--").
      if (lab && lab[0]) {
        const int labScale = 2;
        const uint16_t labInk = redBtn ? (uint16_t)0xFFFF : ink;
        drawKeyGlyph(cx, cy, lab, labInk, inner, labScale);
      }
    };

    for (int i = 0; i < kSlotCount; i++) {
      if (!slotVisible(i)) {
        continue;
      }
      drawArcadeBtn(i, isArrowSlot(i));
    }

    // Commodore 64 logo bitmap above the D-pad (90° CCW).
    {
      int lcx, lcy, lw, lh;
      logoPortrait(lcx, lcy, lw, lh);
      if (lw > 0 && lh > 0 && C64_LOGO_W > 0 && C64_LOGO_H > 0) {
        const int ppx = lcx - lw / 2;
        const int ppy = lcy - lh / 2;
        int x0, y0, x1, y1;
        portraitToFb(ppx, ppy, x0, y0);
        portraitToFb(ppx + lw - 1, ppy + lh - 1, x1, y1);
        const int dx = x0 < x1 ? x0 : x1;
        const int dy = y0 < y1 ? y0 : y1;
        // 90° CCW: src(sx,sy) → dest(sy, W-1-sx); out size H×W.
        for (int sy = 0; sy < C64_LOGO_H; sy++) {
          for (int sx = 0; sx < C64_LOGO_W; sx++) {
            const int nx = sy;
            const int ny = C64_LOGO_W - 1 - sx;
            const int fx = dx + nx;
            const int fy = dy + ny;
            if (fx < 0 || fy < 0 || fx >= (int)CrowPanelHw::PANEL_W ||
                fy >= (int)CrowPanelHw::PANEL_H) {
              continue;
            }
            fb[fy * CrowPanelHw::PANEL_W + fx] =
                C64_LOGO_RGB565[sy * C64_LOGO_W + sx];
          }
        }
      }
    }

    // Profile bank: JOY / USER1 / USER2 / USER3 along the bottom.
    {
      auto p2fbRect = [&](int px, int py, int pw, int ph, int &x, int &y,
                          int &w, int &h) {
        int x0, y0, x1, y1;
        portraitToFb(px, py, x0, y0);
        portraitToFb(px + pw - 1, py + ph - 1, x1, y1);
        x = x0 < x1 ? x0 : x1;
        y = y0 < y1 ? y0 : y1;
        w = (x0 > x1 ? x0 : x1) - x + 1;
        h = (y0 > y1 ? y0 : y1) - y + 1;
      };
      const int active = activeProfile();
      for (int i = 0; i < kProfileCount; i++) {
        int px, py, pw, ph, x, y, w, h;
        profilePortrait(i, px, py, pw, ph);
        if (pw <= 0 || ph <= 0) {
          continue;
        }
        p2fbRect(px, py, pw, ph, x, y, w, h);
        const bool on = (i == active);
        const bool pressed = (shi == kProfileHitBase + i);
        const uint16_t faceP = on ? faceOn : 0xC618;
        const uint16_t edgeP = pressed ? 0xFFFF : beigeEdge;
        fillRoundRectFb(x, y, w, h, 8, faceP, edgeP);
        drawKeyGlyph(x + w / 2, y + h / 2, profileLabel(i), ink, faceP, 1);
      }
    }
  }

  if (CrowPanelSetup::active()) {
    drawSetupOverlay();
  }

  CrowPanelHw::clearKeyboardRedraw();
}

void CrowPanelDisplay::fillC64Background(uint16_t color16) {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  // Only the left C64 region (keyboard lives on the right).
  for (uint16_t y = 0; y < CrowPanelHw::PANEL_H; y++) {
    uint16_t *row = fb + y * CrowPanelHw::PANEL_W;
    for (uint16_t x = 0; x < CrowPanelHw::DST_W; x++) {
      row[x] = color16;
    }
  }
}

void CrowPanelDisplay::init() {
  pinMode(Config::LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(Config::LCD_BACKLIGHT_PIN, Config::LCD_BACKLIGHT_ON_LEVEL);

  Board *b = new Board();
  if (!b->init() || !b->begin()) {
    Serial.println("[CrowPanelDisplay] panel init/begin FAILED");
    while (true) {
      delay(1000);
    }
  }
  CrowPanelHw::setBoard(b);
  CrowPanelHw::setLcd(b->getLCD());
  CrowPanelHw::setTouch(b->getTouch());
  if (!CrowPanelHw::lcd()) {
    Serial.println("[CrowPanelDisplay] no LCD");
    while (true) {
      delay(1000);
    }
  }

  pinMode(Config::LCD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(Config::LCD_BACKLIGHT_PIN, Config::LCD_BACKLIGHT_ON_LEVEL);

  uint16_t *fb = (uint16_t *)heap_caps_malloc(
      CrowPanelHw::PANEL_W * CrowPanelHw::PANEL_H * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fb) {
    Serial.println("[CrowPanelDisplay] PSRAM fb alloc failed");
    while (true) {
      delay(1000);
    }
  }
  CrowPanelHw::setFb(fb);
  memset(fb, 0, CrowPanelHw::PANEL_W * CrowPanelHw::PANEL_H * sizeof(uint16_t));
  fillC64Background(0);
  drawKeyboardOverlay();
  CrowPanelHw::present();
  oldFrameColor = 0;
  Serial.printf(
      "[CrowPanelDisplay] ready %dx%d; C64 4:3 90CCW %dx%d; KB x=%d w=%d "
      "cell=%d\n",
      CrowPanelHw::PANEL_W, CrowPanelHw::PANEL_H, CrowPanelHw::DST_W,
      CrowPanelHw::DST_H, CrowPanelHw::KB_X0, CrowPanelHw::KB_W, crowKeyCell());
}

void CrowPanelDisplay::drawFrame(uint8_t frameColor) {
  if (frameColor == oldFrameColor || !CrowPanelHw::framebuffer()) {
    return;
  }
  oldFrameColor = frameColor;
  fillC64Background(c64Colors[frameColor & 0x0f]);
}

void CrowPanelDisplay::drawBitmap(const uint8_t *bitmap) {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb || !bitmap) {
    return;
  }

  // 90° CCW: (sx,sy) -> (sy, SRC_W-1-sx), then scale into DST while keeping
  // aspect ratio.
  for (uint16_t dy = 0; dy < CrowPanelHw::DST_H; dy++) {
    uint16_t ry =
        (uint16_t)((uint32_t)dy * CrowPanelHw::ROT_H / CrowPanelHw::DST_H);
    uint16_t sx = (uint16_t)(CrowPanelHw::SRC_W - 1 - ry);
    uint16_t *dst = fb + ((CrowPanelHw::ORIGIN_Y + dy) * CrowPanelHw::PANEL_W) +
                    CrowPanelHw::ORIGIN_X;
    for (uint16_t dx = 0; dx < CrowPanelHw::DST_W; dx++) {
      uint16_t rx =
          (uint16_t)((uint32_t)dx * CrowPanelHw::ROT_W / CrowPanelHw::DST_W);
      uint16_t sy = rx;
      uint8_t pix = bitmap[sy * CrowPanelHw::SRC_W + sx];
      dst[dx] = c64Colors[pix & 0x0f];
    }
  }

  CrowPanelHw::tickDriveActivity();
  if (CrowPanelHw::keyboardNeedsRedraw()) {
    drawKeyboardOverlay();
  }
  CrowPanelHw::present();
}

#endif
