/*
 CrowPanel 10.1" MIPI-DSI display driver for T-HMI-C64.
*/
#include "../Config.h"
#ifdef USE_CROWPANEL_DISPLAY

#include "CrowPanelDisplay.h"
#include "CrowPanelDirBrowser.h"
#include "CrowPanelFileBrowser.h"
#include "CrowPanelHw.h"
#include "CrowPanelKeyLayout.h"
#include "../roms/charset.h"
#include "board_config.h"
#include "esp_panel_board_custom_conf.h"
#include "esp_panel_drivers_conf.h"
#include "esp_utils_conf.h"
#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <atomic>
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
                                    uint16_t fg, uint16_t bg) {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb || !label) {
    return;
  }
  // Labels are rotated 90° CCW. Portrait left→right == landscape −Y, so
  // advance upward in the framebuffer for upright reading.
  int scale = 1;
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

void CrowPanelDisplay::drawKeyboardOverlay() {
  uint16_t *fb = CrowPanelHw::framebuffer();
  if (!fb) {
    return;
  }
  const uint16_t bg = 0x3186;
  const uint16_t face = 0x73ae;
  const uint16_t faceHi = 0xb5d6;
  const uint16_t faceMod = 0x064a;
  const uint16_t faceOn = 0xe74e;
  const uint16_t faceCtrl = 0x4a69; // T-HMI control strip
  const uint16_t ink = 0x0000;

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
    if (crowIsCtrlKind(k.kind)) {
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
    fillRect(fb, x, y, w, h, col);
    fillRect(fb, x, y, w, 1, ink);
    fillRect(fb, x, y + h - 1, w, 1, ink);
    fillRect(fb, x, y, 1, h, ink);
    fillRect(fb, x + w - 1, y, 1, h, ink);
    drawKeyGlyph(x + w / 2, y + h / 2, crowKeyFaceLabel(k, shifted), ink,
                 col);
  }

  // Disk name label to the right of SPACE (cols 13-17, row 4).
  {
    CrowKeyDef diskSlot{13, 4, 5, "", nullptr, CrowKeyKind::None, {0xff, 0xff, 0}};
    int x, y, w, h;
    crowKeyRect(diskSlot, x, y, w, h);
    const char *disk = CrowPanelHw::attachedDisk();
    const uint16_t diskFace = disk && disk[0] ? faceOn : faceCtrl;
    fillRect(fb, x, y, w, h, diskFace);
    fillRect(fb, x, y, w, 1, ink);
    fillRect(fb, x, y + h - 1, w, 1, ink);
    fillRect(fb, x, y, 1, h, ink);
    fillRect(fb, x + w - 1, y, 1, h, ink);
    drawKeyGlyph(x + w / 2, y + h / 2, (disk && disk[0]) ? disk : "----", ink,
                 diskFace);
    // Drive activity LED (red) while sectors are being read.
    if (CrowPanelHw::driveReading()) {
      const uint16_t led = 0xF800; // bright red
      fillCircle(fb, x + w / 2, y + 8, 5, led);
    }
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
