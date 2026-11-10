/*
 CrowPanel LIST window: attached .d64 directory with scrollbar + close.
*/
#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include "CrowPanelDirBrowser.h"
#include "CrowPanelHw.h"
#include <cstring>

namespace CrowPanelDirBrowser {
static bool s_active = false;
static int s_count = 0;
static int s_scroll = 0;
static char s_names[kMaxFiles][kNameLen];
static char s_types[kMaxFiles][kTypeLen];
static char s_title[17] = "";
static char s_status[20] = "";

bool active() { return s_active; }

void close() {
  s_active = false;
  CrowPanelHw::requestKeyboardRedraw();
}

void clear() {
  s_count = 0;
  s_scroll = 0;
  memset(s_names, 0, sizeof(s_names));
  memset(s_types, 0, sizeof(s_types));
  s_title[0] = '\0';
  s_status[0] = '\0';
}

void open() {
  clear();
  strncpy(s_status, "READING...", sizeof(s_status) - 1);
  s_active = true;
  CrowPanelHw::requestKeyboardRedraw();
}

void setTitle(const char *title) {
  memset(s_title, 0, sizeof(s_title));
  if (title) {
    strncpy(s_title, title, sizeof(s_title) - 1);
  }
  CrowPanelHw::requestKeyboardRedraw();
}

void setStatus(const char *status) {
  memset(s_status, 0, sizeof(s_status));
  if (status) {
    strncpy(s_status, status, sizeof(s_status) - 1);
  }
  CrowPanelHw::requestKeyboardRedraw();
}

void setEntries(char names[][kNameLen], char types[][kTypeLen], int count) {
  clear();
  if (count < 0) {
    count = 0;
  }
  if (count > kMaxFiles) {
    count = kMaxFiles;
  }
  s_count = count;
  for (int i = 0; i < count; i++) {
    memcpy(s_names[i], names[i], kNameLen);
    memcpy(s_types[i], types[i], kTypeLen);
  }
  CrowPanelHw::requestKeyboardRedraw();
}

int count() { return s_count; }
int scroll() { return s_scroll; }
const char *title() { return s_title; }
const char *status() { return s_status; }

const char *nameAt(int index) {
  if (index < 0 || index >= s_count) {
    return nullptr;
  }
  return s_names[index];
}

const char *typeAt(int index) {
  if (index < 0 || index >= s_count) {
    return nullptr;
  }
  return s_types[index];
}

void scrollBy(int delta) {
  if (s_count <= kVisibleRows) {
    s_scroll = 0;
    CrowPanelHw::requestKeyboardRedraw();
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

static void portraitFromTouch(int tx, int ty, int &px, int &py) {
  px = (int)CrowPanelHw::PANEL_H - 1 - ty;
  py = tx - (int)CrowPanelHw::KB_X0;
}

Hit hitTest(int tx, int ty) {
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
      return Hit::Close;
    }
    if (px < 2 * third) {
      return Hit::Up;
    }
    return Hit::Down;
  }

  // Scrollbar track on the right of the list area.
  if (py >= kHeaderH && py < footerY && px >= w - kPad - kScrollW) {
    if (s_count > kVisibleRows) {
      int listH = footerY - kHeaderH;
      int rel = py - kHeaderH;
      int maxScroll = s_count - kVisibleRows;
      s_scroll = (rel * maxScroll) / (listH > 1 ? listH : 1);
      if (s_scroll < 0) {
        s_scroll = 0;
      }
      if (s_scroll > maxScroll) {
        s_scroll = maxScroll;
      }
      CrowPanelHw::requestKeyboardRedraw();
      return Hit::ScrollTrack;
    }
  }

  return Hit::None;
}

} // namespace CrowPanelDirBrowser

#endif
