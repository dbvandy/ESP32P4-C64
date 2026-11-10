/*
 CrowPanel D64 file picker state + SD scan + hit testing.
*/
#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include "CrowPanelFileBrowser.h"
#include "CrowPanelHw.h"
#include "../fs/SDMMCFile.h"
#include <cctype>
#include <cstring>
#include <string>

namespace CrowPanelFileBrowser {
static bool s_active = false;
static int s_count = 0;
static int s_scroll = 0;
static int s_lastFile = -1;
static char s_names[kMaxFiles][kNameLen];

static bool endsWithD64(const std::string &n) {
  if (n.size() < 4) {
    return false;
  }
  char e0 = (char)tolower((unsigned char)n[n.size() - 4]);
  char e1 = (char)tolower((unsigned char)n[n.size() - 3]);
  char e2 = (char)tolower((unsigned char)n[n.size() - 2]);
  char e3 = (char)tolower((unsigned char)n[n.size() - 1]);
  return e0 == '.' && e1 == 'd' && e2 == '6' && e3 == '4';
}

bool active() { return s_active; }

void close() {
  s_active = false;
  s_lastFile = -1;
  CrowPanelHw::requestKeyboardRedraw();
}

int count() { return s_count; }
int scroll() { return s_scroll; }
int fileIndex() { return s_lastFile; }

const char *nameAt(int index) {
  if (index < 0 || index >= s_count) {
    return nullptr;
  }
  return s_names[index];
}

void scrollBy(int delta) {
  if (s_count <= kVisibleRows) {
    s_scroll = 0;
    return;
  }
  int maxScroll = s_count - kVisibleRows;
  s_scroll += delta;
  if (s_scroll < 0) {
    s_scroll = 0;
  }
  if (s_scroll > maxScroll) {
    s_scroll = maxScroll;
  }
  CrowPanelHw::requestKeyboardRedraw();
}

bool refreshFromSd() {
  s_count = 0;
  s_scroll = 0;
  memset(s_names, 0, sizeof(s_names));
  SDMMCFile probe;
  if (!probe.init()) {
    return false;
  }
  std::string name;
  bool start = true;
  while (s_count < kMaxFiles) {
    if (!probe.listnextentry(name, start)) {
      break;
    }
    start = false;
    if (name.empty()) {
      break;
    }
    if (!endsWithD64(name)) {
      continue;
    }
    size_t n = name.size();
    if (n >= (size_t)kNameLen) {
      n = (size_t)kNameLen - 1;
    }
    memcpy(s_names[s_count], name.c_str(), n);
    s_names[s_count][n] = '\0';
    s_count++;
  }
  return s_count > 0;
}

void portraitRectToFb(int px, int py, int pw, int ph, int &x, int &y, int &w,
                      int &h) {
  x = (int)CrowPanelHw::KB_X0 + py;
  y = (int)CrowPanelHw::PANEL_H - (px + pw);
  w = ph;
  h = pw;
}

void open() {
  refreshFromSd();
  s_active = true;
  s_lastFile = -1;
  CrowPanelHw::requestKeyboardRedraw();
}

static void portraitFromTouch(int tx, int ty, int &px, int &py) {
  px = (int)CrowPanelHw::PANEL_H - 1 - ty;
  py = tx - (int)CrowPanelHw::KB_X0;
}

Hit hitTest(int tx, int ty) {
  s_lastFile = -1;
  if (!s_active || tx < (int)CrowPanelHw::KB_X0) {
    return Hit::None;
  }
  int px, py;
  portraitFromTouch(tx, ty, px, py);
  if (px < 0 || py < 0 || px >= (int)CrowPanelHw::KB_PORTRAIT_W ||
      py >= (int)CrowPanelHw::KB_PORTRAIT_H) {
    return Hit::None;
  }

  const int w = CrowPanelHw::KB_PORTRAIT_W;
  const int h = CrowPanelHw::KB_PORTRAIT_H;
  const int footerY = h - kFooterH;

  if (py >= footerY) {
    int third = w / 3;
    if (px < third) {
      return Hit::Cancel;
    }
    if (px < 2 * third) {
      return Hit::Up;
    }
    return Hit::Down;
  }

  if (py < kHeaderH) {
    return Hit::None;
  }

  int listTop = kHeaderH + 4;
  int rel = py - listTop;
  if (rel < 0) {
    return Hit::None;
  }
  int row = rel / kRowH;
  if (row < 0 || row >= kVisibleRows) {
    return Hit::None;
  }
  int idx = s_scroll + row;
  if (idx < 0 || idx >= s_count) {
    return Hit::None;
  }
  s_lastFile = idx;
  return Hit::File;
}

} // namespace CrowPanelFileBrowser

#endif
