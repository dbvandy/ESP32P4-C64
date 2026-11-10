/*
 CrowPanel on-screen directory window for the attached .d64 (LIST key).
*/
#ifndef CROWPANEL_DIR_BROWSER_H
#define CROWPANEL_DIR_BROWSER_H

#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include <cstdint>

namespace CrowPanelDirBrowser {
constexpr int kMaxFiles = 64;
constexpr int kNameLen = 17;
constexpr int kTypeLen = 4;
constexpr int kVisibleRows = 9;
constexpr int kPad = 8;
constexpr int kHeaderH = 36;
constexpr int kFooterH = 56;
constexpr int kRowH = 44;
constexpr int kScrollW = 28;

bool active();
void open(); // shows window; entries filled by LISTD64DIR
void close();
void clear();
void setTitle(const char *title);
void setStatus(const char *status); // e.g. "NO DISK"
void setEntries(char names[][kNameLen], char types[][kTypeLen], int count);

int count();
int scroll();
void scrollBy(int delta);
const char *nameAt(int index);
const char *typeAt(int index);
const char *title();
const char *status();

enum class Hit : int8_t {
  None = 0,
  Close,
  Up,
  Down,
  ScrollTrack, // jump scroll
};

Hit hitTest(int tx, int ty);
} // namespace CrowPanelDirBrowser

#endif
#endif
