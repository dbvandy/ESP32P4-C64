/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 For the complete text of the GNU General Public License see
 http://www.gnu.org/licenses/.
*/
#ifndef FLOPPY_H
#define FLOPPY_H

#include "CPU6502.h"
#include "IDebugBus.h"
#include "fs/FileDriver.h"
#include "platform/PlatformManager.h"
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class Floppy : public CPU6502 {
private:
  static constexpr uint8_t sectorsPerTrack[41] = {
      0,  21, 21, 21, 21, 21, 21, 21, 21, 21,
      21, 21, 21, 21, 21, 21, 21, 21,        // 1–17
      19, 19, 19, 19, 19, 19, 19,            // 18–24
      18, 18, 18, 18, 18, 18,                // 25–30
      17, 17, 17, 17, 17, 17, 17, 17, 17, 17 // 31–40
  };

  struct BufferMeta {
    bool dirty = false;
    uint8_t track = 0;
    uint8_t sector = 0;
  };

  struct Channel {
    std::unique_ptr<FileDriver> file;
    uint8_t buffernr;
    bool hasChannelName;
    bool isOpen = false;
    // True after OPEN "#" / U1 / B-R: buffer holds a raw sector, not a file chain.
    bool rawBlock = false;
    // Contiguous PRG/SEQ payload (link bytes stripped). Avoids sector-stitch
    // skew on large files like Ultima MAP* (4098 bytes → $1000).
    bool seqActive = false;
    uint16_t seqIdx = 0;
    std::vector<uint8_t> seqData;
    std::string seqName;
    uint16_t bufferidx;
    uint16_t buffersize;
  };

  std::unique_ptr<FileDriver> d64file;
  uint8_t *buffer[5];
  BufferMeta bufferMeta[5];
  uint8_t errmessage[48];
  uint8_t errmessageidx;
  uint16_t freeBlocks;
  uint8_t track;
  uint8_t sector;
  uint8_t startTrack;
  uint8_t startSector;
  uint8_t diriterstate;
  uint8_t dirTrack;
  uint8_t dirSector;
  uint16_t diriteraddr;
  Channel channels[16];
  std::string name;
  uint8_t device = 8;
  bool listening = false;
  bool talking = false;
  uint8_t currentSecondary = 0;
  bool collectName = false;
  // After LISTEN+SECOND on an OPEN "#" channel: next iecout bytes are PRINT#.
  bool receivingRawData = false;
  bool triggererrorchannel = false;
  bool triggercmdchannel = false;
  IDebugBus *debugBus;

  uint8_t ram[0x800];

  int64_t calcOffset(uint8_t track, uint8_t sector) {
    int64_t off = 0;
    for (uint8_t t = 1; t < track; ++t) {
      off += static_cast<int64_t>(sectorsPerTrack[t]) * 256;
    }
    off += static_cast<int64_t>(sector) * 256;
    return off;
  }

  // When True 1541 is active, use its in-memory D64 (one shared image).
  bool mediaReadSector(uint8_t track, uint8_t sector, uint8_t *buf);
  bool mediaWriteSector(uint8_t track, uint8_t sector, const uint8_t *buf);

  void initIterateDirectoryBlk() {
    track = dirTrack;
    sector = dirSector;
  }

  // variables used outside this method:
  // startTrack, startSector, buf, track
  template <typename Callback>
  bool iterateDirectoryBlk(const std::string &filename, uint8_t *buf,
                           Callback cb) {
    if (track != 0) {
      if (!mediaReadSector(track, sector, buf)) {
        PlatformManager::getInstance().log(LOG_ERROR, "Floppy",
                                           "unsuccessful sector read");
        return false;
      }
      track = buf[0];
      sector = buf[1];
      for (uint8_t e = 0; e < 8; e++) {
        uint8_t base = e * 32;
        uint8_t fileType = buf[base + 2];
        uint8_t fileTypeE = fileType & 0x0f;
        startTrack = buf[base + 3];
        startSector = buf[base + 4];
        std::string fname;
        for (uint8_t i = 0; i < 16; i++) {
          uint8_t b = buf[base + 5 + i];
          if (b == 160) {
            break;
          }
          fname.push_back(b);
        }
        if ((fileTypeE < 1) || (fileTypeE > 4)) {
          continue;
        }
        uint16_t blocks = buf[base + 30] + (buf[base + 31] << 8);
        if (cb(fname, filename, blocks, fileType)) {
          return true;
        }
      }
    }
    return false;
  }

  bool readNextDirBlk();
  bool readNextFileBlk();
  bool handleCmdChannel();
  bool directLoad();
  void initChannels();
  void initAttach();
  void logDebugInfo();
  void exeSubroutine(uint16_t regpc);
  void setError(uint8_t code);
  bool allocateBufferForChannel(uint8_t channel);
  void releaseBufferForChannel(uint8_t channel);
  bool writeBufferToDisk(uint8_t bufferId);
  // Push BAM disk ID into 1541 zero-page so mid-game swaps are visible.
  void applyBamHeaderToDriveRam();
  bool reloadBam();
  // Walk PRG/SEQ chain into a contiguous buffer (no per-sector stitch).
  bool prefetchFileChain(uint8_t channel, uint8_t startT, uint8_t startS);
  void clearSeqChannel(uint8_t channel);
  void verifyMapLoadAgainstHostRam(uint8_t channel);

  // C64 RAM (for post-LOAD MAP verify). Set from C64Sys::init.
  uint8_t *hostRam = nullptr;

public:
  static std::unique_ptr<FileDriver> sysfile;

  Floppy(IDebugBus *debug = nullptr) : debugBus(debug) {}

  bool iecindebug = false;
  static bool fsinitialized;

  bool d64attached = false;
  uint8_t lastStatus = 0;

  void setHostRam(uint8_t *ram) { hostRam = ram; }
  void init(uint8_t device);
  bool attach(const std::string &filename);
  void detach();
  uint8_t iecin();
  void iecout(uint8_t value);
  uint16_t load(const std::string &filename, uint8_t *ram);
  bool save(const std::string &filename, uint8_t *ram, uint16_t startaddr,
            uint16_t endaddr);
  void rmPrgFromFilename(std::string &filename);
  bool listnextentry(std::string &name, bool start);
  // Fill host-side directory window (attached .d64). Returns entry count.
  int listDirectory(char names[][17], char types[][4], int maxEntries);
  // Disk title from BAM (16 chars, may include shifted PETSCII).
  bool diskTitle(char out[17]);

  uint8_t getMem(uint16_t addr) override;
  void setMem(uint16_t addr, uint8_t val) override;
  void run() override;
};
#endif // FLOPPY_H
