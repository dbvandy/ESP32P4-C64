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
#include "Floppy.h"
#include "Config.h"
#include "fs/FileFactory.h"
#include "platform/PlatformManager.h"
#include "roms/1541.h"
#include "true1541/True1541.h"
#include "true1541/disk.h"
#if defined(BOARD_CROWPANEL_P4)
#include "display/CrowPanelHw.h"
#define CROW_DRIVE_ACT() CrowPanelHw::pulseDriveActivity()
#else
#define CROW_DRIVE_ACT()                                                       \
  do {                                                                         \
  } while (0)
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

static const char *TAG = "Floppy";

std::string decodeFileType(uint8_t t) {
  int f = t & 0x0f;
  bool closed = (t & 0x80) != 0;
  bool locked = (t & 0x40) != 0;
  std::string n;
  switch (f) {
  case 1:
    n = "SEQ";
    break;
  case 2:
    n = "PRG";
    break;
  case 3:
    n = "USR";
    break;
  case 4:
    n = "REL";
    break;
  default:
    n = "???";
    break;
  }
  if (!closed)
    n = n + "*";
  if (locked)
    n = n + ">";
  return n;
}

uint16_t countBitsLimited(uint32_t mask, uint8_t limit) {
  uint16_t cnt = 0;
  for (uint8_t i = 0; i < limit; ++i) {
    if (mask & (1u << i)) {
      cnt++;
    }
  }
  return cnt;
}

void Floppy::clearSeqChannel(uint8_t channel) {
  if (channel >= 16) {
    return;
  }
  channels[channel].seqActive = false;
  channels[channel].seqIdx = 0;
  channels[channel].seqData.clear();
  channels[channel].seqName.clear();
}

void Floppy::initChannels() {
  for (uint8_t i = 0; i < 16; i++) {
    auto &ch = channels[i];
    ch.buffernr = 0xff;
    ch.hasChannelName = false;
    ch.isOpen = false;
    ch.rawBlock = false;
    ch.bufferidx = 0;
    ch.buffersize = 0;
    clearSeqChannel(i);
  }
  for (auto &bm : bufferMeta) {
    bm.dirty = false;
    bm.track = 0;
    bm.sector = 0;
  }
  dirTrack = 0;
  dirSector = 0;
}

bool Floppy::prefetchFileChain(uint8_t channel, uint8_t startT,
                               uint8_t startS) {
  clearSeqChannel(channel);
  if (startT < 1 || startT > 40) {
    return false;
  }
  uint8_t t = startT;
  uint8_t s = startS;
  uint8_t secbuf[256];
  int guard = 0;
  channels[channel].seqData.reserve(4096 + 64);
  while (t != 0 && guard++ < 1024) {
    if (s >= sectorsPerTrack[t] || !mediaReadSector(t, s, secbuf)) {
      PlatformManager::getInstance().log(
          LOG_ERROR, TAG, "prefetch: read fail T=%u S=%u", (unsigned)t,
          (unsigned)s);
      clearSeqChannel(channel);
      return false;
    }
    const uint8_t nt = secbuf[0];
    const uint8_t ns = secbuf[1];
    if (nt == 0) {
      // Last sector: data bytes at indices 2..ns inclusive (VICE / 1541).
      if (ns >= 2) {
        channels[channel].seqData.insert(channels[channel].seqData.end(),
                                         secbuf + 2, secbuf + ns + 1);
      }
      break;
    }
    channels[channel].seqData.insert(channels[channel].seqData.end(),
                                     secbuf + 2, secbuf + 256);
    t = nt;
    s = ns;
  }
  channels[channel].seqIdx = 0;
  channels[channel].seqActive = !channels[channel].seqData.empty();
  channels[channel].seqName = name;
  if (channels[channel].seqActive) {
    PlatformManager::getInstance().log(
        LOG_INFO, TAG, "prefetch \"%s\" %u bytes (chain linear)",
        name.c_str(), (unsigned)channels[channel].seqData.size());
  }
  return channels[channel].seqActive;
}

void Floppy::verifyMapLoadAgainstHostRam(uint8_t channel) {
  if (!hostRam || channel >= 16 || !channels[channel].seqActive) {
    return;
  }
  const std::string &nm = channels[channel].seqName;
  if (nm.size() < 3) {
    return;
  }
  // MAP*, MON*, MOB* — map is the terrain skew case.
  const bool isMap = (nm[0] == 'M' || nm[0] == 'm') &&
                     (nm[1] == 'A' || nm[1] == 'a') &&
                     (nm[2] == 'P' || nm[2] == 'p');
  if (!isMap) {
    return;
  }
  const auto &data = channels[channel].seqData;
  if (data.size() < 3) {
    return;
  }
  const uint16_t loadAddr =
      (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  const size_t payload = data.size() - 2;
  size_t mismatches = 0;
  size_t firstDiff = (size_t)-1;
  for (size_t i = 0; i < payload; i++) {
    const uint16_t a = (uint16_t)(loadAddr + (uint16_t)i);
    if (hostRam[a] != data[2 + i]) {
      if (firstDiff == (size_t)-1) {
        firstDiff = i;
      }
      mismatches++;
    }
  }
  // Shift signatures vs disk payload (header kept as tiles / drop first tile).
  size_t shiftPlus1 = 0, shiftMinus1 = 0, hdrAsData = 0;
  if (payload >= 64 && loadAddr == 0x1000) {
    for (size_t i = 0; i + 1 < payload && i < 256; i++) {
      if (hostRam[loadAddr + i] == data[2 + i + 1]) {
        shiftMinus1++; // RAM looks like disk dropped first tile
      }
      if (i + 1 < payload && hostRam[loadAddr + i + 1] == data[2 + i]) {
        shiftPlus1++;
      }
    }
    if (payload >= 2) {
      for (size_t i = 0; i < payload && i < 256; i++) {
        if (hostRam[loadAddr + i] == data[i]) { // incl 2-byte hdr as tiles
          hdrAsData++;
        }
      }
    }
  }
  PlatformManager::getInstance().log(
      LOG_INFO, TAG,
      "MAP verify \"%s\" addr=$%04x file=%u payload=%u mismatches=%u "
      "firstDiff=%d shift-1~%u shift+1~%u hdrAsData~%u",
      nm.c_str(), (unsigned)loadAddr, (unsigned)data.size(),
      (unsigned)payload, (unsigned)mismatches,
      firstDiff == (size_t)-1 ? -1 : (int)firstDiff, (unsigned)shiftMinus1,
      (unsigned)shiftPlus1, (unsigned)hdrAsData);
  if (mismatches == 0) {
    PlatformManager::getInstance().log(LOG_INFO, TAG,
                                       "MAP verify OK — RAM matches disk");
  }
}

void Floppy::initAttach() {
  listening = false;
  talking = false;
  currentSecondary = 0;
  collectName = false;
  receivingRawData = false;
  triggererrorchannel = false;
  triggercmdchannel = false;
  d64attached = false;
}

void Floppy::setError(uint8_t code) {
  const char *text;
  uint8_t errTrack = track;
  uint8_t errSector = sector;
  switch (code) {
  case 0:
    text = "OK";
    errTrack = 0;
    errSector = 0;
    break;
  case 1:
    text = "FILES SCRATCHED";
    break;
  case 25:
    text = "WRITE ERROR";
    break;
  case 30:
    text = "SYNTAX ERROR";
    break;
  case 62:
    text = "FILE NOT FOUND";
    break;
  case 65:
    text = "NO BLOCK";
    break;
  case 66:
    text = "ILLEGAL TRACK OR SECTOR";
    break;
  case 67:
    text = "ILLEGAL SYSTEM T OR S";
    break;
  case 70:
    text = "NO CHANNEL";
    break;
  case 74:
    text = "DISK FULL";
    break;
  default:
    text = "";
    break;
  }
  snprintf((char *)errmessage, sizeof(errmessage), "%02d, %s,%02d,%02d", code,
           text, errTrack, errSector);
}

bool Floppy::allocateBufferForChannel(uint8_t channel) {
  if (channels[channel].buffernr != 0xff) {
    return true;
  }
  if (channel == 15) {
    channels[15].buffernr = 4;
    return true;
  }
  for (uint8_t i = 0; i < 4; i++) {
    bool free = true;
    for (auto &ch : channels) {
      if (ch.buffernr == i) {
        free = false;
        break;
      }
    }
    if (free) {
      channels[channel].buffernr = i;
      return true;
    }
  }
  return false;
}

void Floppy::releaseBufferForChannel(uint8_t channel) {
  verifyMapLoadAgainstHostRam(channel);
  clearSeqChannel(channel);
  uint8_t bnr = channels[channel].buffernr;
  if (bnr >= 5) {
    return;
  }
  // Must flush before dropping the buffer or sector writes are lost.
  writeBufferToDisk(bnr);
  bufferMeta[bnr].dirty = false;
  bufferMeta[bnr].track = 0;
  bufferMeta[bnr].sector = 0;
  channels[channel].buffernr = 0xff;
}

bool Floppy::mediaReadSector(uint8_t track, uint8_t sector, uint8_t *buf) {
  if (!buf) {
    return false;
  }
  if (True1541::isActive() && disk_present()) {
    return disk_image_read_sector(track, sector, buf);
  }
  if (!d64file || !d64file->seek(calcOffset(track, sector), SEEK_SET)) {
    return false;
  }
  return d64file->read(buf, 256) == 256;
}

bool Floppy::mediaWriteSector(uint8_t track, uint8_t sector,
                              const uint8_t *buf) {
  if (!buf) {
    return false;
  }
  if (True1541::isActive() && disk_present()) {
    return disk_image_write_sector(track, sector, buf);
  }
  if (!d64file || !d64file->seek(calcOffset(track, sector), SEEK_SET)) {
    return false;
  }
  return d64file->write(buf, 256) == 256;
}

bool Floppy::writeBufferToDisk(uint8_t bufferId) {
  if (!bufferMeta[bufferId].dirty) {
    return true;
  }
  if (!d64attached && !(True1541::isActive() && disk_present())) {
    return false;
  }
  const uint8_t t = bufferMeta[bufferId].track;
  const uint8_t s = bufferMeta[bufferId].sector;
  // Never flush a buffer that was never bound to a real sector (PRINT# alone).
  if (t < 1 || t > 40 || s >= sectorsPerTrack[t]) {
    PlatformManager::getInstance().log(
        LOG_WARN, TAG, "skip dirty flush: invalid T=%u S=%u", (unsigned)t,
        (unsigned)s);
    bufferMeta[bufferId].dirty = false;
    return true;
  }
  PlatformManager::getInstance().log(
      LOG_INFO, TAG, "flush buffer %u → T=%u S=%u", (unsigned)bufferId,
      (unsigned)t, (unsigned)s);
  if (!mediaWriteSector(t, s, buffer[bufferId])) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "writeBuffer write failed");
    return false;
  }
  bufferMeta[bufferId].dirty = false;
  CROW_DRIVE_ACT();
  return true;
}

bool Floppy::fsinitialized = false;
std::unique_ptr<FileDriver> Floppy::sysfile;

void Floppy::init(uint8_t device) {
  this->device = device;
  buffer[0] = &ram[0x300];
  buffer[1] = &ram[0x400];
  buffer[2] = &ram[0x500];
  buffer[3] = &ram[0x600];
  buffer[4] = &ram[0x700];
  if (!fsinitialized) {
    sysfile = FileSys::create();
    fsinitialized = sysfile->init();
    d64file = FileSys::create();
    for (auto &ch : channels) {
      ch.file = FileSys::create();
    }
  }
  initChannels();
  initAttach();
}

void Floppy::applyBamHeaderToDriveRam() {
  // BAM disk ID at +0xA2/+0xA3. Mirror into 1541 ZP like VICE does on attach
  // so loaders that poll $12/$13 (or header ID $16/$17) see the new disk.
  uint8_t id0 = buffer[4][0xa2];
  uint8_t id1 = buffer[4][0xa3];
  ram[0x12] = id0;
  ram[0x13] = id1;
  ram[0x16] = id0;
  ram[0x17] = id1;
  ram[0x18] = 18; // header track
  ram[0x19] = 0;  // header sector
  ram[0x1c] = 1;  // disk-change flag, drive 0
  PlatformManager::getInstance().log(LOG_INFO, TAG, "disk id %02x %02x", id0,
                                     id1);
}

bool Floppy::reloadBam() {
  if (!mediaReadSector(18, 0, buffer[4])) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "unsuccessful BAM read");
    return false;
  }
  if (buffer[4][0] == 0) {
    return false;
  }
  dirTrack = buffer[4][0];
  dirSector = buffer[4][1];

  int64_t filesize = 0;
  if (d64file) {
    filesize = d64file->size();
  } else if (True1541::isActive() && disk_present()) {
    filesize = (int64_t)D64_STD_SIZE;
  }
  uint8_t detectedTracks = (filesize == 196608) ? 40 : 35;
  freeBlocks = 0;
  for (uint8_t t = 1; t <= detectedTracks; t++) {
    if (t == 18) {
      continue;
    }
    uint8_t base = (t - 1) * 4;
    uint32_t mask = static_cast<uint32_t>(buffer[4][base + 5]) |
                    (static_cast<uint32_t>(buffer[4][base + 6]) << 8) |
                    (static_cast<uint32_t>(buffer[4][base + 7]) << 16);
    freeBlocks += countBitsLimited(mask, sectorsPerTrack[t]);
  }
  applyBamHeaderToDriveRam();
  return true;
}

bool Floppy::attach(const std::string &filename) {
  // Flush any pending writes from the previous image before swapping.
  for (uint8_t i = 0; i < 5; i++) {
    writeBufferToDisk(i);
  }
  initChannels();
  initAttach();
  std::string d64filename = Config::PATH + filename;
  d64file->close();
  // Must open read/write for U2/B-W and dirty-buffer flush. rb-only means
  // sector writes will fail.
  if (!d64file->open(d64filename, "r+b") &&
      !d64file->open(d64filename, "rb+")) {
    PlatformManager::getInstance().log(
        LOG_ERROR, "Floppy",
        "cannot open %s for read/write — disk writes disabled",
        d64filename.c_str());
    if (!d64file->open(d64filename, "rb")) {
      PlatformManager::getInstance().log(
          LOG_ERROR, "Floppy", "cannot open file %s", d64filename.c_str());
      return false;
    }
  }
  if (!reloadBam()) {
    d64file->close();
    return false;
  }
  d64attached = true;
  name.clear();
  collectName = false;
  setError(0);
  lastStatus = 0;
  CROW_DRIVE_ACT(); // brief LED blink on insert / swap
  PlatformManager::getInstance().log(LOG_INFO, TAG, "attached %s",
                                     d64filename.c_str());
  return true;
}

void Floppy::detach() {
  for (uint8_t i = 0; i < 5; i++) {
    writeBufferToDisk(i);
  }
  initChannels();
  initAttach();
  d64attached = false;
  d64file->close();
  ram[0x1c] = 1; // disk removed
}

void addDirectoryLine(uint8_t *buf, uint16_t &idx, uint16_t &addr,
                      uint16_t blocks, const std::string &name,
                      const std::string &type) {
  addr += 32;
  uint16_t endidx = idx + 32;
  // nextline pointer
  buf[idx++] = addr & 0xff;
  buf[idx++] = (addr >> 8) & 0xff;
  // line number = number of blocks
  buf[idx++] = blocks & 0xff;
  buf[idx++] = (blocks >> 8) & 0xff;
  int padding = 4 - std::to_string((int)blocks).length();
  for (uint8_t i = 0; i < padding; i++) {
    buf[idx++] = ' ';
  }
  // name + file type
  buf[idx++] = '"';
  for (char c : name) {
    buf[idx++] = static_cast<uint8_t>(c);
  }
  buf[idx++] = '"';
  padding = 17 - name.length();
  for (uint8_t i = 0; i < padding; i++) {
    buf[idx++] = ' ';
  }
  for (char c : type) {
    buf[idx++] = static_cast<uint8_t>(c);
  }
  while (idx < endidx - 1) {
    buf[idx++] = ' ';
  }
  // line terminator
  buf[idx++] = 0;
}

bool Floppy::readNextDirBlk() {
  CROW_DRIVE_ACT();
  uint8_t *buf = buffer[channels[0].buffernr];
  uint16_t idx = 0;
  switch (diriterstate) {
  case 1: {
    diriteraddr = 0x0801; // basic start
    buf[idx++] = diriteraddr & 0xff;
    buf[idx++] = (diriteraddr >> 8) & 0xff;
    diriteraddr += 30;
    // nextline pointer
    buf[idx++] = diriteraddr & 0xff;
    buf[idx++] = (diriteraddr >> 8) & 0xff;
    // disk name
    std::string diskName;
    for (uint8_t i = 0; i < 16; i++) {
      diskName.push_back(buffer[4][0x90 + i]);
    }
    // line number = number of blocks
    buf[idx++] = 0;
    buf[idx++] = 0;
    // name
    buf[idx++] = 18; // reverse on
    std::string line = "\"" + diskName + "\" ";
    for (char c : line) {
      buf[idx++] = static_cast<uint8_t>(c);
    }
    for (uint8_t i = 0; i < 5; i++) {
      uint8_t ch = buffer[4][0xa2 + i];
      buf[idx++] = ch >= 128 ? ch - 128 : ch;
    }
    buf[idx++] = 0;
    channels[0].buffersize = 32;
    channels[0].bufferidx = 0;
    diriterstate++;
    initIterateDirectoryBlk();
    break;
  }
  case 2: {
    iterateDirectoryBlk(name, buf,
                        [&](const std::string &name, const std::string &search,
                            const uint16_t blocks, const uint8_t fileType) {
                          addDirectoryLine(buf, idx, diriteraddr, blocks, name,
                                           decodeFileType(fileType));
                          return false;
                        });
    channels[0].buffersize = idx;
    channels[0].bufferidx = 0;
    if (track == 0) {
      diriterstate++;
    }
    break;
  }
  case 3: {
    diriteraddr += 30;
    buf[idx++] = diriteraddr & 0xff;
    buf[idx++] = (diriteraddr >> 8) & 0xff;
    buf[idx++] = freeBlocks & 0xff;
    buf[idx++] = (freeBlocks >> 8) & 0xff;
    std::string line = "BLOCKS FREE.";
    for (char c : line) {
      buf[idx++] = static_cast<uint8_t>(c);
    }
    for (uint8_t i = 0; i < 13; i++) {
      buf[idx++] = 32;
    }
    buf[idx++] = 0;
    buf[idx++] = 0;
    channels[0].buffersize = idx;
    channels[0].bufferidx = 0;
    diriterstate++;
    break;
  }
  case 4: {
    lastStatus = 0x40; // EOI
    channels[0].buffersize = 0;
    channels[0].bufferidx = 0;
    return true;
  }
  }
  return false;
}

bool Floppy::readNextFileBlk() {
  if (track != 0) {
    CROW_DRIVE_ACT();
    uint8_t bnr = channels[currentSecondary].buffernr;
    if (bnr >= 5) {
      lastStatus = 0x02;
      setError(70);
      return true;
    }
    uint8_t *buf = buffer[bnr];
    if (!mediaReadSector(track, sector, buf)) {
      track = 0;
      lastStatus = 0x40; // EOI
      return true;
    }
    uint8_t oldtrack = track;
    uint8_t oldsector = sector;
    track = buf[0];
    sector = buf[1];
    bnr = channels[currentSecondary].buffernr;
    bufferMeta[bnr].track = oldtrack;
    bufferMeta[bnr].sector = oldsector;
    if ((oldtrack == track) && (oldsector == sector)) {
      PlatformManager::getInstance().log(
          LOG_ERROR, TAG, "IECIN error: track/sector repeating!");
      track = 0;
      lastStatus = 0x02;
      setError(67);
      return true;
    }
    if (track == 0) {
      channels[currentSecondary].buffersize = sector + 1;
    } else {
      channels[currentSecondary].buffersize = 256;
    }
    channels[currentSecondary].bufferidx = 2;
  } else {
    lastStatus = 0x40; // EOI
    return true;
  }
  return false;
}

bool Floppy::directLoad() {
  if (!channels[0].isOpen) {
    PlatformManager::getInstance().log(LOG_INFO, TAG, "file not found");
    lastStatus = 0x42;
    setError(62);
    return true;
  }
  uint8_t *buf = buffer[channels[0].buffernr];
  channels[0].bufferidx = 0;
  channels[0].buffersize = channels[0].file->read(buf, 256);
  if (channels[0].buffersize == 0) {
    lastStatus = 0x40; // EOI
    return true;
  }
  return false;
}

enum class CommandType { UNKNOWN, MEMORY, TEXT };

struct GeneralCommand {
  CommandType type = CommandType::UNKNOWN;
  std::string command;
  std::vector<uint8_t> rawArgs;
  std::vector<std::string> textArgs;
};

bool isMemoryCmd(const std::string &data) {
  return data.size() >= 3 && data[0] == 'M' && data[1] == '-' &&
         std::isalpha(static_cast<unsigned char>(data[2]));
}

bool isTextCmd(const std::string &data) {
  return !data.empty() && std::isalpha(static_cast<unsigned char>(data[0]));
}

std::string formatUInt8ArgsToHex(const std::vector<uint8_t> &args) {
  std::stringstream ss;
  ss << std::hex << std::setfill('0');
  for (uint8_t i = 0; i < args.size(); i++) {
    ss << std::setw(2) << static_cast<unsigned int>(args[i]);
    if (i < args.size() - 1) {
      ss << ",";
    }
  }
  return ss.str();
}

std::string formatStringArgsToHex(const std::vector<std::string> &args) {
  std::stringstream ss;
  for (uint8_t i = 0; i < args.size(); ++i) {
    ss << args[i];
    if (i < args.size() - 1) {
      ss << ",";
    }
  }
  return ss.str();
}

std::optional<GeneralCommand> handleCommand(const std::string &data) {
  GeneralCommand cmd;
  if (isMemoryCmd(data)) {
    cmd.type = CommandType::MEMORY;
    cmd.command = data.substr(0, 3);
    cmd.rawArgs.assign(data.begin() + 3, data.end());
    return cmd;
  } else if (isTextCmd(data)) {
    cmd.type = CommandType::TEXT;
    auto pos = data.find_first_of(": ");
    if (pos == std::string::npos) {
      cmd.command = data;
      return cmd;
    }
    cmd.command = data.substr(0, pos);
    std::string params = data.substr(pos + 1);
    params.erase(0, params.find_first_not_of(" "));
    std::replace(params.begin(), params.end(), ',', ' ');
    std::stringstream ss(params);
    std::string token;
    while (ss >> token) {
      cmd.textArgs.push_back(token);
    }
    return cmd;
  }
  return std::nullopt;
}

bool Floppy::handleCmdChannel() {
  auto cmd = handleCommand(name);
  if (cmd) {
    if (cmd->type == CommandType::MEMORY) {
      uint16_t addr = cmd->rawArgs[0] + (cmd->rawArgs[1] << 8);
      PlatformManager::getInstance().log(
          LOG_INFO, TAG, "command: %s, args: %s, addr: %d",
          cmd->command.c_str(), formatUInt8ArgsToHex(cmd->rawArgs).c_str(),
          addr);
      if (cmd->command == "M-R") {
        if (channels[15].buffernr >= 5) {
          channels[15].buffernr = 4;
        }
        uint8_t *dst = buffer[channels[15].buffernr];
        uint16_t bufsize = cmd->rawArgs[2];
        if (bufsize == 0) {
          bufsize = 256;
        }
        for (uint16_t i = 0; i < bufsize && (addr + i) <= 0xffff; i++) {
          dst[i] = getMem(addr + i);
        }
        channels[15].bufferidx = 0;
        channels[15].buffersize = bufsize;
      } else if (cmd->command == "M-W") {
        // M-W: addr_lo addr_hi count data... → drive RAM (job buffers).
        if (cmd->rawArgs.size() < 3) {
          lastStatus = 0x02;
          setError(30);
          return true;
        }
        uint8_t count = cmd->rawArgs[2];
        if (count == 0) {
          count = 1;
        }
        PlatformManager::getInstance().log(
            LOG_INFO, TAG, "M-W addr=%04x count=%u payload=%u", addr,
            (unsigned)count, (unsigned)(cmd->rawArgs.size() - 3));
        for (uint8_t i = 0; i < count; i++) {
          if ((size_t)(3 + i) >= cmd->rawArgs.size()) {
            break;
          }
          uint16_t a = (uint16_t)(addr + i);
          uint8_t v = cmd->rawArgs[3 + i];
          setMem(a, v);
          // If this lands in a job buffer page, mark that buffer dirty.
          if (a >= 0x0300 && a < 0x0800) {
            uint8_t bnr = (uint8_t)((a - 0x0300) >> 8);
            if (bnr < 5) {
              bufferMeta[bnr].dirty = true;
            }
          }
        }
        setError(0);
        lastStatus = 0;
      } else if (cmd->command == "M-E") {
        exeSubroutine(addr);
      }
    } else if (cmd->type == CommandType::TEXT) {
      PlatformManager::getInstance().log(
          LOG_INFO, TAG, "command: %s, args: %s", cmd->command.c_str(),
          formatStringArgsToHex(cmd->textArgs).c_str());
      if ((cmd->command == "U1") || (cmd->command == "UA") ||
          (cmd->command == "B-R") || (cmd->command == "B-E") ||
          (cmd->command == "U2") || (cmd->command == "UB") ||
          (cmd->command == "B-W")) {
        if (cmd->textArgs.size() < 4) {
          PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                             "block cmd needs 4 args");
          lastStatus = 0x02;
          setError(30);
          return true;
        }
        uint8_t track1 = static_cast<uint8_t>(std::stoul(cmd->textArgs[2]));
        uint8_t sector1 = static_cast<uint8_t>(std::stoul(cmd->textArgs[3]));
        uint8_t secch1 = static_cast<uint8_t>(std::stoul(cmd->textArgs[0]));
        if (secch1 >= 16) {
          PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                             "invalid channel: %d", secch1);
          lastStatus = 0x02;
          setError(70);
          return true;
        }
        if (track1 < 1 || track1 > 40 ||
            sector1 >= sectorsPerTrack[track1]) {
          lastStatus = 0x02;
          setError(66);
          return true;
        }
        if (!d64attached) {
          lastStatus = 0x02;
          setError(74);
          return true;
        }
        if (channels[secch1].buffernr >= 5) {
          if (!allocateBufferForChannel(secch1)) {
            PlatformManager::getInstance().log(
                LOG_ERROR, TAG, "no free buffer for channel %d", secch1);
            lastStatus = 0x02;
            setError(70);
            return true;
          }
        }
        PlatformManager::getInstance().log(
            LOG_INFO, TAG, "track1 = %d, sector1 = %d, secch1=%d, buffernr=%d",
            (int)track1, (int)sector1, (int)secch1,
            (int)channels[secch1].buffernr);
        const bool isWrite = (cmd->command == "U2") || (cmd->command == "UB") ||
                             (cmd->command == "B-W");
        // Random-access "#" / U1/U2: always the full 256-byte sector image —
        // never shift by +2/254 (that corrupted writes).
        constexpr uint16_t kSectorBytes = 256;
        uint8_t bnr = channels[secch1].buffernr;
        if (bufferMeta[bnr].dirty && !isWrite) {
          lastStatus = 0x02;
          setError(65);
          return true;
        }
        uint8_t *buf = buffer[bnr];
        bufferMeta[bnr].track = track1;
        bufferMeta[bnr].sector = sector1;
        CROW_DRIVE_ACT();
        channels[secch1].isOpen = true;
        channels[secch1].rawBlock = true;
        if (cmd->command == "B-E") {
          if (!mediaReadSector(track1, sector1, buf)) {
            lastStatus = 0x02;
            setError(66);
            return true;
          }
          PlatformManager::getInstance().log(LOG_INFO, TAG, "exeSubroutine %d",
                                             buf - ram);
          exeSubroutine(buf - ram);
          bufferMeta[bnr].dirty = true;
        } else if (isWrite) {
          PlatformManager::getInstance().log(
              LOG_INFO, TAG, "sector WRITE %s ch=%u T=%u S=%u",
              cmd->command.c_str(), (unsigned)secch1, (unsigned)track1,
              (unsigned)sector1);
          if (!mediaWriteSector(track1, sector1, buf)) {
            lastStatus = 0x02;
            setError(25);
            return true;
          }
          bufferMeta[bnr].dirty = false;
          setError(0);
          lastStatus = 0;
        } else {
          // Match VICE vdrive + 1541 ROM:
          // U1/UA: bufptr=0, length=256 (full sector).
          // B-R:   bufptr=1, length=buffer[0]+1 (ROM quirk / LSTCHR).
          if (!mediaReadSector(track1, sector1, buf)) {
            lastStatus = 0x02;
            setError(66);
            return true;
          }
          if (cmd->command == "B-R") {
            channels[secch1].bufferidx = 1;
            channels[secch1].buffersize =
                (uint16_t)((uint16_t)buf[0] + 1u);
            if (channels[secch1].buffersize > kSectorBytes) {
              channels[secch1].buffersize = kSectorBytes;
            }
          } else {
            channels[secch1].bufferidx = 0; // U1 / UA / B-E
            channels[secch1].buffersize = kSectorBytes;
          }
          bufferMeta[bnr].dirty = false;
          setError(0);
          lastStatus = 0;
        }
      } else if (cmd->command == "B-P") {
        uint8_t secch1 = static_cast<uint8_t>(std::stoul(cmd->textArgs[0]));
        if (secch1 >= 16) {
          PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                             "invalid channel: %d", secch1);
          lastStatus = 0x02;
          setError(70);
          return true;
        }
        if (channels[secch1].buffernr >= 5) {
          if (!allocateBufferForChannel(secch1)) {
            PlatformManager::getInstance().log(
                LOG_ERROR, TAG, "no free buffer for channel %d", secch1);
            lastStatus = 0x02;
            setError(70);
            return true;
          }
        }
        uint16_t offset = static_cast<uint16_t>(std::stoul(cmd->textArgs[1]));
        if (cmd->textArgs.size() > 2) {
          offset += static_cast<uint16_t>(std::stoul(cmd->textArgs[2])) << 8;
        }
        channels[secch1].bufferidx = offset;
        PlatformManager::getInstance().log(
            LOG_INFO, TAG, "B-P channel %d, offset %d", secch1, offset);
      } else if (cmd->command == "I" || cmd->command == "I0" ||
                 cmd->command == "I1") {
        // Initialize: re-read BAM + refresh disk ID (common after a swap).
        if (!d64attached || !reloadBam()) {
          lastStatus = 0x02;
          setError(74);
          return true;
        }
        setError(0);
        lastStatus = 0;
      } else {
        PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                           "unknown command: %s", name.c_str());
      }
    } else {
      PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                         "unknown command type");
    }
  } else {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "unknown command type");
  }
  return false;
}

uint8_t Floppy::iecin() {
  if (!talking) {
    return 0;
  }
  if (triggererrorchannel) {
    if (errmessageidx < (int)sizeof(errmessage)) {
      return errmessage[errmessageidx++];
    }
    lastStatus = 0x40; // EOI
    triggererrorchannel = false;
    PlatformManager::getInstance().log(LOG_INFO, TAG, "errmessage sent!");
    return 0;
  }
  uint8_t cursec = currentSecondary;
  // PlatformManager::getInstance().log(
  //     LOG_INFO, TAG, "iecin, currentSecondary=%d", currentSecondary);
  if (name == "$") {
    cursec = 0;
  }
  lastStatus = 0;
  setError(0);
  if (!channels[cursec].isOpen) {
    lastStatus = 0x42; // file not found
    setError(62);
    return 0;
  }
  // Linear PRG/SEQ prefetch: exact file bytes, EOI on last (no sector stitch).
  if (channels[cursec].seqActive) {
    const auto &data = channels[cursec].seqData;
    if (channels[cursec].seqIdx >= data.size()) {
      lastStatus = 0x40;
      return 0;
    }
    const uint8_t byte = data[channels[cursec].seqIdx++];
    if (channels[cursec].seqIdx >= data.size()) {
      lastStatus = 0x40; // EOI on last data byte
    }
    if (iecindebug) {
      PlatformManager::getInstance().log(LOG_INFO, TAG, "iecin seq byte: %x",
                                         byte);
    }
    return byte;
  }
  // send bytes to the c64
  if (channels[cursec].bufferidx >= channels[cursec].buffersize) {
    if (channels[cursec].rawBlock) {
      // VICE vdrive_iec_read: exhausted "#" → bufptr=1 + EOF.
      channels[cursec].bufferidx = 1;
      lastStatus = 0x40;
      return 0;
    }
    if (d64attached) {
      if (triggercmdchannel) {
        triggercmdchannel = false;
      } else if (name == "$") {
        if (readNextDirBlk()) {
          return 0;
        }
      } else {
        if (readNextFileBlk()) {
          return 0;
        }
      }
    } else {
      if (directLoad()) {
        return 0;
      }
    }
  }
  if (channels[cursec].buffernr >= 5) {
    if (cursec == 15) {
      channels[15].buffernr = 4;
    } else {
      if (!allocateBufferForChannel(cursec)) {
        PlatformManager::getInstance().log(
            LOG_ERROR, TAG, "iecin: cannot allocate buffer for channel %d",
            cursec);
        lastStatus = 0x02;
        setError(70);
        return 0;
      }
    }
  }
  uint8_t *buf = buffer[channels[cursec].buffernr];
  uint8_t byte = buf[channels[cursec].bufferidx];
  channels[cursec].bufferidx++;
  // EOI on the last data byte (real 1541 / VICE). For sequential PRG/SEQ only
  // when this is the final chain sector (track link already 0). Mid-file
  // sectors must not EOI or LOAD stops early.
  if (channels[cursec].bufferidx >= channels[cursec].buffersize) {
    if (channels[cursec].rawBlock) {
      channels[cursec].bufferidx = 1;
      lastStatus = 0x40;
    } else if (d64attached && track == 0) {
      lastStatus = 0x40;
    }
  }
  if (iecindebug) {
    PlatformManager::getInstance().log(LOG_INFO, TAG, "iecin byte: %x", byte);
  }
  return byte;
}

bool wildcard_match(const char *text, const char *pattern) {
  const char *retry_text = nullptr;
  const char *retry_pattern = nullptr;
  while (*text) {
    if (*pattern == '*') {
      retry_pattern = ++pattern;
      retry_text = text;
    } else if (*pattern == '?' || *text == *pattern) {
      pattern++;
      text++;
    } else if (retry_text) {
      pattern = retry_pattern;
      text = ++retry_text;
    } else {
      return false;
    }
  }
  while (*pattern == '*') {
    pattern++;
  }
  return !*pattern;
}

void Floppy::iecout(uint8_t value) {
  // PRINT# into OPEN "#" after LISTEN+SECOND (no ATN — that broke LOAD).
  // 0x3F ends the transfer (same convention collectName uses for UNLISTEN).
  if (receivingRawData && listening && !collectName) {
    if (value == 0x3f) {
      receivingRawData = false;
      listening = false;
      lastStatus &= 0x40;
      setError(0);
      PlatformManager::getInstance().log(
          LOG_INFO, TAG, "PRINT# done ch=%u size=%u",
          (unsigned)currentSecondary,
          (unsigned)channels[currentSecondary].buffersize);
      return;
    }
    if (currentSecondary != 15 && channels[currentSecondary].isOpen &&
        channels[currentSecondary].rawBlock) {
      uint8_t bnr = channels[currentSecondary].buffernr;
      if (bnr < 5) {
        uint16_t idx = channels[currentSecondary].bufferidx;
        uint16_t lim = channels[currentSecondary].buffersize;
        if (lim == 0 || lim > 256) {
          lim = 256;
          channels[currentSecondary].buffersize = 256;
        }
        if (idx < lim) {
          buffer[bnr][idx] = value;
          uint16_t next = (uint16_t)(idx + 1);
          // VICE: PRINT# into "#" wraps pointer to 0 at length (not 1).
          channels[currentSecondary].bufferidx =
              (next >= lim) ? 0 : next;
          if (bufferMeta[bnr].track >= 1) {
            bufferMeta[bnr].dirty = true;
          }
          if ((idx & 0x0f) == 0) {
            PlatformManager::getInstance().log(
                LOG_INFO, TAG, "PRINT# ch=%u buf[%u]=%02x ...",
                (unsigned)currentSecondary, (unsigned)idx, value);
          }
        }
      }
    }
    return;
  }

  if (collectName) {
    if (value == 0x3f) {
      // end of filename
      collectName = false;
      PlatformManager::getInstance().log(LOG_INFO, TAG, "name = %s!",
                                         name.c_str());
      if (d64attached) {
        if (name == "$") {
          allocateBufferForChannel(0);
          channels[0].isOpen = true;
          diriterstate = 1;
        } else if (currentSecondary == 15) {
          if (listening) {
            PlatformManager::getInstance().log(LOG_INFO, TAG,
                                               "triggercmdchannel");
            handleCmdChannel();
            triggercmdchannel = true;
          }
        } else if (name == "#" ||
                   (name.size() >= 1 && name[0] == '#')) {
          // OPEN n,8,ch,"#" — allocate a raw buffer for U1/B-R sector I/O.
          if (!allocateBufferForChannel(currentSecondary)) {
            lastStatus = 0x02;
            setError(70);
            channels[currentSecondary].isOpen = false;
          } else {
            lastStatus = 0;
            setError(0);
            channels[currentSecondary].isOpen = true;
            channels[currentSecondary].rawBlock = true;
            // VICE/1541 OPEN "#": bufptr=1, length=256 (firmware $cc0f).
            // U1 sets ptr=0 after read; Ultima ID-write does B-P:2 0 before
            // PRINT#. Do not force 0 here — that diverges from a real 1541.
            channels[currentSecondary].bufferidx = 1;
            channels[currentSecondary].buffersize = 256;
            uint8_t bnr = channels[currentSecondary].buffernr;
            if (bnr < 5) {
              memset(buffer[bnr], 0, 256);
              bufferMeta[bnr].dirty = false;
              bufferMeta[bnr].track = 0;
              bufferMeta[bnr].sector = 0;
            }
            PlatformManager::getInstance().log(
                LOG_INFO, TAG, "open raw buffer channel %d", currentSecondary);
          }
        } else {
          // PlatformManager::getInstance().log(
          //     LOG_INFO, TAG, "iecout, currentSecondary=%d",
          //     currentSecondary);
          lastStatus = 0;
          setError(0);
          channels[currentSecondary].rawBlock = false;
          initIterateDirectoryBlk();
          bool found = false;
          while ((track != 0) && (!found)) {
            uint8_t *buf = buffer[channels[currentSecondary].buffernr];
            found = iterateDirectoryBlk(
                name, buf,
                [&](const std::string &name, const std::string &search,
                    const uint16_t blocks, const uint8_t fileType) {
                  return wildcard_match(name.c_str(), search.c_str());
                });
          }
          if (!found) {
            lastStatus = 0x42; // file not found
            setError(62);
            channels[currentSecondary].isOpen = false;
            clearSeqChannel(currentSecondary);
          } else {
            track = startTrack;
            sector = startSector;
            channels[currentSecondary].isOpen = true;
            channels[currentSecondary].bufferidx = 0;
            channels[currentSecondary].buffersize = 0;
            // Prefetch full chain so MAP/PRG loads cannot skew across sectors.
            if (!prefetchFileChain(currentSecondary, startTrack, startSector)) {
              // Fall back to per-sector readNextFileBlk path.
              clearSeqChannel(currentSecondary);
              track = startTrack;
              sector = startSector;
            } else {
              // Chain already consumed into seqData; mark EOF for legacy path.
              track = 0;
              sector = 0;
            }
          }
        }
      } else { // "direct" load
        for (auto &c : name) {
          c = tolower(c);
        }
        lastStatus = 0;
        setError(0);
        std::string filename = Config::PATH + name + ".prg";
        if (!channels[0].file->open(filename, "rb")) {
          PlatformManager::getInstance().log(LOG_INFO, TAG, "cannot open file");
          channels[0].isOpen = false;
          lastStatus = 0x42; // file not found
          setError(62);
        } else {
          channels[0].isOpen = true;
          channels[0].bufferidx = 0;
          channels[0].buffersize = 0;
          allocateBufferForChannel(0);
        }
      }
    } else {
      // get next char for filename
      name += static_cast<char>(value);
    }
  }
  if (!collectName) {
    uint8_t cmd = value & 0xf0;
    if (value == 0x20 + device) {
      listening = true;
      talking = false;
      receivingRawData = false;
    } else if (value == 0x3f) {
      lastStatus &= 0x40;
      setError(0);
      listening = false;
      receivingRawData = false;
    } else if (value == 0x40 + device) {
      talking = true;
      listening = false;
      receivingRawData = false;
    } else if (value == 0x5f) {
      triggererrorchannel = false;
      talking = false;
      receivingRawData = false;
    } else if (cmd == 0x60) {
      currentSecondary = value & 0x0f;
      if ((currentSecondary == 15) &&
          (channels[15].bufferidx >= channels[15].buffersize)) {
        if (talking) {
          errmessageidx = 0;
          channels[15].hasChannelName = false;
          channels[15].isOpen = true;
          triggererrorchannel = true;
        } else if (listening) {
          allocateBufferForChannel(15);
          channels[15].hasChannelName = true;
          channels[15].isOpen = true;
          name = "";
          collectName = true;
        }
        receivingRawData = false;
      } else if (listening && currentSecondary != 15 &&
                 channels[currentSecondary].isOpen &&
                 channels[currentSecondary].rawBlock) {
        // Arm PRINT# capture for this "#" channel (U2/B-W payload).
        receivingRawData = true;
        PlatformManager::getInstance().log(
            LOG_INFO, TAG, "PRINT# arm ch=%u idx=%u",
            (unsigned)currentSecondary,
            (unsigned)channels[currentSecondary].bufferidx);
      } else {
        receivingRawData = false;
      }
    } else if (cmd == 0xe0) {
      currentSecondary = value & 0x0f;
      releaseBufferForChannel(currentSecondary);
      channels[currentSecondary].hasChannelName = false;
      channels[currentSecondary].isOpen = false;
      channels[currentSecondary].rawBlock = false;
      receivingRawData = false;
      name = "";
    } else if (cmd == 0xf0) {
      currentSecondary = value & 0x0f;
      receivingRawData = false;
      if (!allocateBufferForChannel(currentSecondary)) {
        channels[currentSecondary].isOpen = false;
        PlatformManager::getInstance().log(
            LOG_ERROR, TAG, "no free buffer for channel %d", currentSecondary);
        return;
      }
      channels[currentSecondary].hasChannelName = true;
      channels[currentSecondary].isOpen = true;
      channels[currentSecondary].rawBlock = false;
      name = "";
      collectName = true;
    }
  }
}

uint16_t Floppy::load(const std::string &filename, uint8_t *ram) {
  std::string path = Config::PATH + filename;
  if (!sysfile->open(path, "rb")) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "cannot open file %s",
                                       path.c_str());
    return 0;
  }
  PlatformManager::getInstance().feedWDT();
  uint8_t buf[256];
  if (sysfile->read(buf, 2) != 2) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG,
                                       "not a prg file (header too short)");
    return 0;
  }
  uint8_t addrLow = buf[0], addrHigh = buf[1];
  uint16_t addr = addrLow | (addrHigh << 8);
  uint16_t bytesRead;
  PlatformManager::getInstance().feedWDT();
  while ((bytesRead = sysfile->read(buf, sizeof(buf))) > 0) {
    PlatformManager::getInstance().feedWDT();
    for (uint16_t i = 0; i < bytesRead; i++) {
      ram[addr++] = buf[i];
    }
  }
  sysfile->close();
  PlatformManager::getInstance().feedWDT();
  return addr;
}

bool Floppy::save(const std::string &filename, uint8_t *ram, uint16_t startaddr,
                  uint16_t endaddr) {
  std::string path = Config::PATH + filename;
  if (!sysfile->open(path, "wb")) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "cannot open file %s",
                                       path.c_str());
    return false;
  }
  uint8_t buf[2];
  buf[0] = startaddr & 0xff;
  buf[1] = (startaddr >> 8) & 0xff;
  if (sysfile->write(buf, 2) != 2) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "could not write file");
    return false;
  }
  uint16_t length = endaddr - startaddr;
  if (sysfile->write(ram + startaddr, length) != length) {
    PlatformManager::getInstance().log(LOG_ERROR, TAG, "could not write file");
    return false;
  }
  sysfile->close();
  return true;
}

void Floppy::rmPrgFromFilename(std::string &filename) {
  if (filename.size() > 4 &&
      filename.compare(filename.size() - 4, 4, ".prg") == 0) {
    filename.erase(filename.size() - 4);
  }
}

bool Floppy::listnextentry(std::string &name, bool start) {
  return sysfile->listnextentry(name, start);
}

bool Floppy::diskTitle(char out[17]) {
  if (!out) {
    return false;
  }
  out[0] = '\0';
  if (!d64attached) {
    return false;
  }
  // Refresh BAM so the title matches the currently attached image.
  if (!mediaReadSector(18, 0, buffer[4])) {
    return false;
  }
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t b = buffer[4][0x90 + i];
    if (b == 0xa0) {
      out[i] = '\0';
      return true;
    }
    // Map common PETSCII to ASCII for the overlay.
    if (b >= 0xC1 && b <= 0xDA) {
      b = (uint8_t)(b - 0x80); // shifted A-Z → A-Z
    }
    out[i] = (b >= 32 && b < 127) ? (char)b : '?';
    out[i + 1] = '\0';
  }
  return true;
}

int Floppy::listDirectory(char names[][17], char types[][4], int maxEntries) {
  if (!d64attached || !names || !types || maxEntries <= 0) {
    return 0;
  }
  if (dirTrack == 0) {
    if (!reloadBam()) {
      return 0;
    }
  }
  initIterateDirectoryBlk();
  int n = 0;
  uint8_t tmp[256];
  while (track != 0 && n < maxEntries) {
    iterateDirectoryBlk(
        "", tmp,
        [&](const std::string &fname, const std::string & /*search*/,
            const uint16_t /*blocks*/, const uint8_t fileType) {
          if (n >= maxEntries) {
            return true;
          }
          size_t len = fname.size();
          if (len > 16) {
            len = 16;
          }
          memcpy(names[n], fname.c_str(), len);
          names[n][len] = '\0';
          for (size_t i = 0; i < len; i++) {
            char c = names[n][i];
            if (c >= 'a' && c <= 'z') {
              names[n][i] = (char)(c - 'a' + 'A');
            }
          }
          std::string t = decodeFileType(fileType);
          size_t tl = t.size();
          if (tl > 3) {
            tl = 3;
          }
          memcpy(types[n], t.c_str(), tl);
          types[n][tl] = '\0';
          n++;
          return false;
        });
  }
  return n;
}

uint8_t Floppy::getMem(uint16_t addr) {
  if (addr < 0x0800) {
    return ram[addr];
  } else if (addr >= 0xc000) {
    return rom_1541[addr - 0xc000];
  }
  return 0;
}

void Floppy::setMem(uint16_t addr, uint8_t val) {
  if (addr < 0x0800) {
    ram[addr] = val;
  }
}

void Floppy::logDebugInfo() {
  PlatformManager::getInstance().log(LOG_INFO, TAG,
                                     "pc: %2x, cmd: %s, a: %x, x: %x, y: %x, "
                                     "sp: %x, sr: %x, arg1 = %x, arg2 = %x",
                                     pc, cmdName[getMem(pc)], a, x, y, sp, sr,
                                     getMem(pc + 1), getMem(pc + 2));
}

void Floppy::exeSubroutine(uint16_t regpc) {
  sp = 0xff;
  uint16_t i = 20000;
  uint8_t tsp = sp;
  pc = regpc;
  while (i != 0) {
    i--;
    logDebugInfo();
    uint8_t nextopc = getMem(pc++);
    if ((nextopc == 0x00) || ((sp == tsp) && (nextopc == 0x60))) { // brk or rts
      break;
    }
    execute(nextopc);
  }
}

void Floppy::run() {}
