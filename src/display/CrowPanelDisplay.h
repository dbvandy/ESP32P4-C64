/*
 CrowPanel 10.1" MIPI-DSI display driver for T-HMI-C64.
 Rotates the C64 320x200 bitmap 90° CCW (aspect preserved) on the left;
 the on-screen C64 keyboard is also rotated 90° CCW on the right (= below
 the display when the panel is held upright for that rotation).
*/
#ifndef CROWPANEL_DISPLAY_H
#define CROWPANEL_DISPLAY_H

#include "../Config.h"
#ifdef USE_CROWPANEL_DISPLAY
#include "DisplayDriver.h"
#include <cstdint>

class CrowPanelDisplay : public DisplayDriver {
private:
  uint8_t oldFrameColor = 0xff;
  void fillC64Background(uint16_t color16);
  void drawKeyboardOverlay();
  void drawFileBrowserOverlay();
  void drawDirBrowserOverlay();
  void drawSetupOverlay();
  void drawPadSelectOverlay();
  void drawKeyGlyph(int x, int y, const char *label, uint16_t fg, uint16_t bg,
                    int scale = 1);
  void drawPortraitButton(int px, int py, int pw, int ph, const char *label,
                          uint16_t face, uint16_t ink);

public:
  void init() override;
  void drawFrame(uint8_t frameColor) override;
  void drawBitmap(const uint8_t *bitmap) override;
};

#endif
#endif // CROWPANEL_DISPLAY_H
