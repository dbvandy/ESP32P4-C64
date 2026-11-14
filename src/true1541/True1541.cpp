#include "True1541.h"

#include "Config.h"
#include "drive_hw.h"
#include "disk.h"
#include "platform/PlatformManager.h"
#if defined(BOARD_CROWPANEL_P4)
#include "display/CrowPanelHw.h"
#include "fs/SDMMCFile.h"
#include <esp_heap_caps.h>
#else
#include "fs/FileFactory.h"
#endif
#include <cstring>
#include <memory>
#include <string>

static const char *TAG = "True1541";

namespace True1541 {

// CIA2 $DD00 Port A IEC bits (VICE / AAY / hardware — NOT the PRG bit shift):
// PA3 ATN out, PA4 CLK out, PA5 DATA out, PA6 CLK in, PA7 DATA in.
// Each OUT drives a 7406 inverter: register 1 (or float-high) pulls the bus low.
static constexpr uint8_t kAtnOut = 0x08;
static constexpr uint8_t kClkOut = 0x10;
static constexpr uint8_t kDataOut = 0x20;
static constexpr uint8_t kClkIn = 0x40;
static constexpr uint8_t kDataIn = 0x80;

static std::string gMountRel;
static uint8_t gHostPulls;
static bool gActive;

void setActive(bool on) { gActive = on; }
bool isActive() { return gActive; }

static uint8_t hostPullMask(uint8_t pa, uint8_t ddra) {
  // Pin level seen by the 7406: driven outs | floating inputs (~DDR → high).
  const uint8_t out = (uint8_t)((pa & ddra) | (uint8_t)~ddra);
  uint8_t pulls = 0;
  if (out & kDataOut) {
    pulls |= IEC_DATA;
  }
  if (out & kClkOut) {
    pulls |= IEC_CLK;
  }
  if (out & kAtnOut) {
    pulls |= IEC_ATN;
  }
  return pulls;
}

void init() {
  // Always bring up the drive CPU/VIA — IEC must work even if GCR PSRAM fails.
  const bool psramOk = disk_psram_init();
  if (!psramOk) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "PSRAM alloc for GCR/D64 failed");
  }
  drive_init();
  drive_reset();
  PlatformManager::getInstance().log(
      LOG_INFO, TAG, "hardware 1541 ready (psram=%d present=%d)", psramOk ? 1 : 0,
      drive_present() ? 1 : 0);
}

void reset() { drive_reset(); }

void spinUp() {
  // ~2.5s of drive time — past motor-on after reset bump so DOS reaches the
  // ATN-wait idle with motor/LED off (0.5s left the spindle running).
  if (!drive_present()) {
    return;
  }
  drive_run_phi2(2500000u);
  drive_force_idle();
  PlatformManager::getInstance().log(
      LOG_INFO, TAG, "drive spun up (motor=%d led=%d)", drive_motor_on() ? 1 : 0,
      drive_activity_led() ? 1 : 0);
}

bool attachD64(const std::string &path) {
  // Match Floppy/SDMMCFile: relative path; driver prefixes /sdcard/.
  std::string rel = path;
  if (rel.rfind("/sdcard/", 0) == 0) {
    rel = rel.substr(8);
  }
  if (rel.find('/') == std::string::npos &&
      rel.find('\\') == std::string::npos) {
    rel = std::string(Config::PATH) + rel;
  }

  if (!disk_psram_init()) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "attach aborted: no PSRAM buffers");
    return false;
  }

#if defined(BOARD_CROWPANEL_P4)
  SDMMCFile file;
  FileDriver *fd = &file;
#else
  auto owned = FileSys::create();
  FileDriver *fd = owned.get();
#endif
  if (!fd || !fd->open(rel, "rb")) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "open failed: %s",
                                       rel.c_str());
    return false;
  }

#if defined(BOARD_CROWPANEL_P4)
  uint8_t *buf = (uint8_t *)heap_caps_malloc(
      D64_ERR_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  uint8_t *buf = (uint8_t *)malloc(D64_ERR_SIZE);
#endif
  if (!buf) {
    fd->close();
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "temp D64 buffer alloc failed");
    return false;
  }

  size_t n = fd->read(buf, D64_ERR_SIZE);
  fd->close();

  size_t mountLen = 0;
  if (n == D64_STD_SIZE || n == D64_ERR_SIZE) {
    mountLen = n;
  } else if (n > D64_STD_SIZE) {
    // Padded / 40-track images: use the standard 35-track prefix.
    mountLen = D64_STD_SIZE;
    PlatformManager::getInstance().log(
        LOG_INFO, TAG, "D64 size %u — mounting first %u bytes", (unsigned)n,
        (unsigned)D64_STD_SIZE);
  } else {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "D64 too small (%u) for %s", (unsigned)n,
                                       rel.c_str());
#if defined(BOARD_CROWPANEL_P4)
    heap_caps_free(buf);
#else
    free(buf);
#endif
    return false;
  }

  disk_unmount();
  const bool ok = disk_mount_image(buf, mountLen);
#if defined(BOARD_CROWPANEL_P4)
  heap_caps_free(buf);
#else
  free(buf);
#endif
  if (!ok) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "mount_image failed: %s",
                                       rel.c_str());
    gMountRel.clear();
    return false;
  }
  gMountRel = rel;

  drive_reset();
  spinUp();

  // Seed DSKID/HEADER like VICE/Floppy so DOS ID checks match the image.
  uint8_t id1 = 0, id2 = 0;
  disk_get_id(&id1, &id2);
  drive_bus_poke(0x0012, id1);
  drive_bus_poke(0x0013, id2);
  drive_bus_poke(0x0016, id1);
  drive_bus_poke(0x0017, id2);
  drive_bus_poke(0x0018, 18);
  drive_bus_poke(0x0019, 0);
  // Do NOT set WPSW ($1C)=1 here: idle DOS treats that as disk-change and
  // keeps re-initing (motor/LED ~1s on/off). Stable disk → WPSW clear.
  drive_bus_poke(0x001C, 0);
  // PRGTRK/PRGSEC/PRGDRV: LOAD"*" means "last file" when $7E != 0.
  drive_bus_poke(0x007E, 0);   // PRGTRK
  drive_bus_poke(0x026E, 0);   // PRGDRV
  drive_bus_poke(0x026F, 0);   // PRGSEC
  drive_force_idle();

  uint8_t probe[256];
  const bool bamOk = disk_image_read_sector(18, 0, probe);
  PlatformManager::getInstance().log(
      LOG_INFO, TAG, "mounted %s (%u bytes) id=%02x%02x bam=%s motor=%d",
      rel.c_str(), (unsigned)mountLen, id1, id2, bamOk ? "ok" : "FAIL",
      drive_motor_on() ? 1 : 0);
  return true;
}

void detach() {
  writeback();
  disk_unmount();
  gMountRel.clear();
}

bool diskPresent() { return disk_present(); }

void iecHostWrite(uint8_t pa, uint8_t ddra) {
  gHostPulls = hostPullMask(pa, ddra);
  drive_set_iec_ext(gHostPulls);
}

uint8_t iecHostRead(uint8_t pa, uint8_t ddra) {
  // Keep host pulls fresh even when the game only polls $DD00.
  gHostPulls = hostPullMask(pa, ddra);
  drive_set_iec_ext(gHostPulls);
  const uint8_t drivePulls = drive_iec_out();
  const bool dataLow = ((gHostPulls | drivePulls) & IEC_DATA) != 0;
  const bool clkLow = ((gHostPulls | drivePulls) & IEC_CLK) != 0;

  // Start from driven/floating pin levels, then overlay CLK/DATA sense bits.
  uint8_t v = (uint8_t)((pa & ddra) | (uint8_t)~ddra);
  if (dataLow) {
    v &= (uint8_t)~kDataIn;
  } else {
    v |= kDataIn;
  }
  if (clkLow) {
    v &= (uint8_t)~kClkIn;
  } else {
    v |= kClkIn;
  }
  return v;
}

void runPhi2(uint32_t phi2Cycles) {
  if (!drive_present()) {
    return;
  }
  drive_run_phi2(phi2Cycles);
}

bool motorOn() { return drive_motor_on(); }

bool activityLed() { return drive_activity_led(); }

void forceIdle() { drive_force_idle(); }

bool hostAtn() { return (gHostPulls & IEC_ATN) != 0; }

bool isDirty() { return disk_is_dirty(); }

bool writeback() {
  if (!disk_is_dirty()) {
    return true;
  }
  if (gMountRel.empty()) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "writeback: no mount path");
    return false;
  }
  disk_sync_image_from_gcr();
  size_t len = 0;
  const uint8_t *img = disk_image_bytes(&len);
  if (!img || len == 0) {
    return false;
  }

#if defined(BOARD_CROWPANEL_P4)
  SDMMCFile file;
  FileDriver *fd = &file;
#else
  auto owned = FileSys::create();
  FileDriver *fd = owned.get();
#endif
  // Prefer in-place update; fall back to truncate-write.
  if (!fd || !fd->open(gMountRel, "rb+")) {
    if (!fd || !fd->open(gMountRel, "wb")) {
      PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                         "writeback open failed: %s",
                                         gMountRel.c_str());
      return false;
    }
  }
  if (!fd->seek(0, SEEK_SET)) {
    fd->close();
    return false;
  }
  const size_t wrote = fd->write(img, len);
  fd->close();
  if (wrote != len) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "writeback short write %u/%u",
                                       (unsigned)wrote, (unsigned)len);
    return false;
  }
  disk_mark_clean();
  PlatformManager::getInstance().log(LOG_INFO, TAG, "writeback ok %s (%u)",
                                     gMountRel.c_str(), (unsigned)len);
  return true;
}

} // namespace True1541
