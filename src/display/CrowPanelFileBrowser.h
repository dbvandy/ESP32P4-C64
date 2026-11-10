/*
 CrowPanel D64 file picker: SD .d64 list drawn in the keyboard region.
*/
#ifndef CROWPANEL_FILE_BROWSER_H
#define CROWPANEL_FILE_BROWSER_H

#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include <cstdint>

namespace CrowPanelFileBrowser {
constexpr int kMaxFiles = 64;
constexpr int kNameLen = 20; // "abcdefghijklmn.prg" + NUL
constexpr int kVisibleRows = 10;
constexpr int kPad = 8;
constexpr int kHeaderH = 36;
constexpr int kFooterH = 56;
constexpr int kRowH = 44;

bool active();
void open();
void close();
bool refreshFromSd();

int count();
int scroll();
void scrollBy(int delta);
const char *nameAt(int index); // full name including .prg, or nullptr

/** Hit result for a panel touch (landscape coords). */
enum class Hit : int8_t {
  None = 0,
  Cancel,
  Up,
  Down,
  File, // use fileIndex()
};

Hit hitTest(int tx, int ty);
int fileIndex(); // valid after Hit::File

/** Portrait→landscape rect helper for drawing a portrait box. */
void portraitRectToFb(int px, int py, int pw, int ph, int &x, int &y, int &w,
                      int &h);
} // namespace CrowPanelFileBrowser

#endif
#endif
