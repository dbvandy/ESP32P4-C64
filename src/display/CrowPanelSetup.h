/*
 CrowPanel SETUP overlay: brightness + volume sliders, persisted in NVS.
*/
#ifndef CROWPANEL_SETUP_H
#define CROWPANEL_SETUP_H

#include "../Config.h"
#if defined(BOARD_CROWPANEL_P4) && defined(USE_CROWPANEL_DISPLAY)

#include <cstdint>

namespace CrowPanelSetup {

void init(); // load NVS + apply brightness/volume
bool active();
void open();
void close();

// Called every scan while a finger is down (supports slider drag).
void onTouch(int touchX, int touchY);
void onRelease();

uint8_t brightness(); // 0..100
uint8_t volume();     // 0..255

// Portrait panel geometry for drawing (beige window).
void panelPortrait(int &x, int &y, int &w, int &h);
void brightTrackPortrait(int &x, int &y, int &w, int &h);
void volTrackPortrait(int &x, int &y, int &w, int &h);
void closeBtnPortrait(int &x, int &y, int &w, int &h);

} // namespace CrowPanelSetup

#endif
#endif
