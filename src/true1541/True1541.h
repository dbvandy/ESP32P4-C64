/*
 CrowPanel True 1541 hardware drive (separate from fast Floppy).
 Toggle with ExtCmd::TOGGLETRUE1541 / TRUE 1541 key.
*/
#ifndef TRUE1541_H
#define TRUE1541_H

#include <cstdint>
#include <string>

namespace True1541 {

void init();
void reset();
void setActive(bool on); // True 1541 mode owns media / blocks Floppy flushes
bool isActive();
// Run drive cycles so DOS can finish boot after reset/attach (before LOAD).
void spinUp();
bool attachD64(const std::string &absoluteOrRelativePath);
void detach();
bool diskPresent();

// CIA2 $DD00 Port A helpers (DDR-aware). Call only when True mode is active.
void iecHostWrite(uint8_t pa, uint8_t ddra);
uint8_t iecHostRead(uint8_t pa, uint8_t ddra);

// Advance drive clock for elapsed C64 phi2 cycles.
void runPhi2(uint32_t phi2Cycles);
bool motorOn();
bool activityLed();
bool hostAtn();
bool isDirty();
void forceIdle();

// Flush dirty image sectors back to the mounted .d64 on SD.
bool writeback();

} // namespace True1541

#endif
