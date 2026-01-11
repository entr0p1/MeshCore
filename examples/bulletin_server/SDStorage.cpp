#include "SDStorage.h"

SDStorage::SDStorage() : _status(SD_NOT_SUPPORTED), _cs_pin(-1) {}

bool SDStorage::begin(int cs_pin) {
#ifdef ESP32
  #ifdef PIN_SDCARD_CS
    _cs_pin = (cs_pin >= 0) ? cs_pin : PIN_SDCARD_CS;
  #else
    _cs_pin = cs_pin;
  #endif

  if (_cs_pin < 0) {
    Serial.println("SDStorage: No SD CS pin defined for this board");
    _status = SD_NOT_SUPPORTED;
    return false;
  }

  if (!SD.begin(_cs_pin)) {
    Serial.println("SDStorage: SD.begin() failed - card not present or unformatted");
    _status = SD_NOT_PRESENT;
    return false;
  }

  // Check if card is actually accessible
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SDStorage: No SD card detected");
    _status = SD_NOT_PRESENT;
    return false;
  }

  // Create base directory if needed
  if (!SD.exists(SD_BASE_DIR)) {
    if (!SD.mkdir(SD_BASE_DIR)) {
      Serial.println("SDStorage: Failed to create bulletin directory");
      _status = SD_UNFORMATTED;
      return false;
    }
  }

  _status = SD_READY;
  Serial.printf("SDStorage: Ready - %lu KB total, %lu KB free\n",
                (unsigned long)getTotalSpace(), (unsigned long)getFreeSpace());
  return true;
#else
  // Non-ESP32 platforms don't support SD
  _status = SD_NOT_SUPPORTED;
  return false;
#endif
}

uint32_t SDStorage::getTotalSpace() const {
#ifdef ESP32
  if (_status != SD_READY) return 0;
  return SD.totalBytes() / 1024;
#else
  return 0;
#endif
}

uint32_t SDStorage::getUsedSpace() const {
#ifdef ESP32
  if (_status != SD_READY) return 0;
  return SD.usedBytes() / 1024;
#else
  return 0;
#endif
}

uint32_t SDStorage::getFreeSpace() const {
  uint32_t total = getTotalSpace();
  uint32_t used = getUsedSpace();
  return (total > used) ? (total - used) : 0;
}

void SDStorage::formatStorageString(char* dest, size_t len) const {
  switch (_status) {
    case SD_READY: {
      uint32_t used = getUsedSpace();
      uint32_t total = getTotalSpace();
      // Format as KB, MB, or GB depending on size
      if (total >= 1048576) {  // >= 1 GB
        snprintf(dest, len, "%luMB/%luGB",
                 (unsigned long)(used / 1024),
                 (unsigned long)(total / 1048576));
      } else if (total >= 1024) {  // >= 1 MB
        snprintf(dest, len, "%luKB/%luMB",
                 (unsigned long)used,
                 (unsigned long)(total / 1024));
      } else {
        snprintf(dest, len, "%luKB/%luKB",
                 (unsigned long)used, (unsigned long)total);
      }
      break;
    }
    case SD_UNFORMATTED:
      strncpy(dest, "unformatted", len);
      break;
    case SD_NOT_PRESENT:
      strncpy(dest, "no card", len);
      break;
    case SD_NOT_SUPPORTED:
    default:
      strncpy(dest, "Not supported", len);
      break;
  }
  dest[len - 1] = '\0';  // Ensure null termination
}

bool SDStorage::eraseAllData() {
#ifdef ESP32
  if (_status != SD_READY) return false;

  File dir = SD.open(SD_BASE_DIR);
  if (!dir) return false;

  // Delete all files in the directory
  File file = dir.openNextFile();
  while (file) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_BASE_DIR, file.name());
    file.close();
    SD.remove(path);
    file = dir.openNextFile();
  }
  dir.close();

  Serial.println("SDStorage: All data erased");
  return true;
#else
  return false;
#endif
}

void SDStorage::buildPath(char* dest, size_t destLen, const char* filename) const {
  if (filename[0] == '/') {
    snprintf(dest, destLen, "%s%s", SD_BASE_DIR, filename);
  } else {
    snprintf(dest, destLen, "%s/%s", SD_BASE_DIR, filename);
  }
}

File SDStorage::openForRead(const char* filename) {
#ifdef ESP32
  if (_status != SD_READY) return File();
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.open(path, FILE_READ);
#else
  return File();
#endif
}

File SDStorage::openForWrite(const char* filename) {
#ifdef ESP32
  if (_status != SD_READY) return File();
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.open(path, FILE_WRITE);
#else
  return File();
#endif
}

bool SDStorage::exists(const char* filename) {
#ifdef ESP32
  if (_status != SD_READY) return false;
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.exists(path);
#else
  return false;
#endif
}

bool SDStorage::remove(const char* filename) {
#ifdef ESP32
  if (_status != SD_READY) return false;
  char path[64];
  buildPath(path, sizeof(path), filename);
  return SD.remove(path);
#else
  return false;
#endif
}
